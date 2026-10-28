#include "media/CameraChannel.h"

#include "common/Log.h"

#include <chrono>
#include <cstring>

namespace aibox {

namespace {
inline int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

CameraChannel::CameraChannel(uint32_t cam_id, std::string rtsp_url, SourceFactory mk_src,
                             DecoderFactory mk_dec)
    : CameraChannel(cam_id, std::move(rtsp_url), std::move(mk_src), std::move(mk_dec),
                    Params()) {}

CameraChannel::CameraChannel(uint32_t cam_id, std::string rtsp_url, SourceFactory mk_src,
                             DecoderFactory mk_dec, Params p)
    : cam_id_(cam_id),
      rtsp_url_(std::move(rtsp_url)),
      mk_src_(std::move(mk_src)),
      mk_dec_(std::move(mk_dec)),
      p_(p),
      pool_(p.pool_depth, p.frame_w, p.frame_h, kDrmFmtNV12, p.pool_alloc) {}

CameraChannel::~CameraChannel() {
    if (state_.load(std::memory_order_relaxed) != State::STOPPED) stop();
}

void CameraChannel::set_state(State s) { state_.store(s, std::memory_order_relaxed); }

void CameraChannel::start() {
    State expect = State::INIT;
    if (!state_.compare_exchange_strong(expect, State::LOADING)) return;

    src_ = mk_src_(rtsp_url_);
    dec_ = mk_dec_();
    dec_->open(p_.frame_w, p_.frame_h);
    source_opens_.fetch_add(1, std::memory_order_relaxed);
    src_->open();
    last_frame_ms_.store(steady_ms(), std::memory_order_relaxed);

    running_.store(true, std::memory_order_relaxed);
    src_th_ = std::thread(&CameraChannel::src_loop, this);
    worker_th_ = std::thread(&CameraChannel::worker_loop, this);
    wd_th_ = std::thread(&CameraChannel::watchdog_loop, this);
    // 次态 RUNNING 由首帧到达触发（src_loop），保持 LOADING 直至那时
}

void CameraChannel::reconfigure(const std::vector<AlgoCfg>& algos) {
    // 迁移表：RUNNING→RECONFIG→RUNNING。同步执行无 I/O：
    //   drain 本通道 q_ai_（旧算法在途帧全部作废）+ 清 sampler 令牌
    if (state_.load(std::memory_order_relaxed) != State::RUNNING) return;
    set_state(State::RECONFIG);

    size_t drained = q_ai_.drain();  // drain 内部计 out（这些帧不再进 sink）
    (void)drained;
    sampler_.reset();  // 下一帧立即放行（新算法首帧尽快生效）
    {
        std::lock_guard<std::mutex> lk(algos_mu_);
        algos_ = algos;  // 全量替换（set_algos 语义）
    }
    // 通道抽帧率 = 各算法 fps 的最大值（单 Sampler 每通道一档；空表保底 1fps）
    uint8_t fps = 1;
    for (const auto& c : algos)
        if (c.detect_fps > fps) fps = c.detect_fps;
    sampler_.configure(fps);
    set_state(State::RUNNING);  // drain ≤ 队列深 4，远小于 500ms 预算
}

void CameraChannel::stop() {
    State s = state_.load(std::memory_order_relaxed);
    if (s == State::STOPPED) return;

    running_.store(false, std::memory_order_relaxed);
    if (src_) src_->close();  // 解锁阻塞中的 read
    if (src_th_.joinable()) src_th_.join();
    if (worker_th_.joinable()) worker_th_.join();
    if (wd_th_.joinable()) wd_th_.join();
    q_ai_.drain();  // 在途帧归还池（sink 已停，无引用外泄 → in_use 归零）
    // start() 未执行过（如 add_cam 重复 id：构造后被查重析构）时 dec_/src_ 为空——
    // 无条件解引用即 SIGSEGV（板上稳定复现：二次 add_cam 必崩）
    if (dec_) dec_->close();
    if (src_) src_->close();
    set_state(State::STOPPED);
}

void CameraChannel::src_loop() {
    Packet pkt;
    Frame f;
    while (running_.load(std::memory_order_relaxed)) {
        if (!src_->read(pkt)) {
            // 重连是本线程唯一职责（单层重试；businessd/上层不做源重试，防风暴）
            src_->close();
            std::this_thread::sleep_for(std::chrono::milliseconds(p_.reconnect_backoff_ms));
            if (!running_.load(std::memory_order_relaxed)) break;
            src_->open();
            source_opens_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (!dec_->decode(pkt, f)) continue;
        m_dec_.in.fetch_add(1, std::memory_order_relaxed);
        last_frame_ms_.store(steady_ms(), std::memory_order_relaxed);
        if (state_.load(std::memory_order_relaxed) == State::LOADING) {
            set_state(State::RUNNING);  // 迁移表：首帧到达 LOADING→RUNNING
        }

        // 池边：acquire 池空 → drop（背压内存上界行为，绝不等待）
        auto buf = pool_.acquire();
        m_pool_.in.fetch_add(1, std::memory_order_relaxed);
        if (!buf) {
            // 池耗尽 = 下游持有泄漏或分配器故障——静默会让排障全靠猜（板上实测教训）
            if (!pool_warn_once_.exchange(true, std::memory_order_relaxed))
                LOGE("ch", "cam %u pool acquire 失败（池耗尽/分配器故障）", cam_id_);
            m_pool_.drop.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!copy_into_pool(f, buf)) {
            // 拷贝被拒：流尺寸 ≠ 池尺寸（最常见）或 CPU 指针缺失——一次性打印实况
            if (!pool_warn_once_.exchange(true, std::memory_order_relaxed))
                LOGE("ch", "cam %u 帧拷贝被拒: 流 %ux%u(stride %u,vstride %u) ≠ 池 %ux%u"
                          " —— --frame-w/--frame-h 须与子码流实际分辨率一致",
                     cam_id_, f.width, f.height, f.stride_y, f.vstride_y,
                     p_.frame_w, p_.frame_h);
            m_pool_.drop.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // 帧离开解码边/池边（拷贝成功 → 进入抽帧边）：补 out 使两条边守恒
        //（in==out+drop），否则 get_metrics 恒显 out=0，差分定位时误判"断点在此"
        m_dec_.out.fetch_add(1, std::memory_order_relaxed);
        m_pool_.out.fetch_add(1, std::memory_order_relaxed);
        buf->pts = static_cast<int64_t>(pkt.pts_ms);
        // M2: fake 源 pts 单位 ms；板上 M3 RtspPuller 统一归零透传为 us（§12.5）

        // 抽帧边：令牌不通过 → sampled（设计内不推理），fd/内存立即归还
        if (!sampler_.take(steady_us())) {
            m_samp_.in.fetch_add(1, std::memory_order_relaxed);
            m_samp_.sampled.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        buf->frame_seq = frame_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        m_samp_.in.fetch_add(1, std::memory_order_relaxed);
        m_samp_.out.fetch_add(1, std::memory_order_relaxed);  // 离开抽帧边
        q_ai_.push_drop_oldest(std::move(buf));  // 队列边：满弹最旧（newest-wins）
    }
}

bool CameraChannel::copy_into_pool(const Frame& f, const std::shared_ptr<DmaBuffer>& buf) {
    // CPU 拷贝路径（x86/兜底）：stride→紧凑 NV12。板上零拷贝路径（fd 直通）在 M3
    // 由 hw 层短路此函数——通道逻辑不变
    const uint32_t w = p_.frame_w, h = p_.frame_h;
    if (f.width != w || f.height != h || !f.y_ptr || !f.uv_ptr || !buf->map) return false;
    auto* dst = static_cast<uint8_t*>(buf->map);
    for (uint32_t r = 0; r < h; ++r)
        memcpy(dst + static_cast<size_t>(r) * w, f.y_ptr + static_cast<size_t>(r) * f.stride_y, w);
    uint8_t* du = dst + static_cast<size_t>(w) * h;
    for (uint32_t r = 0; r < h / 2; ++r)
        memcpy(du + static_cast<size_t>(r) * w,
               f.uv_ptr + static_cast<size_t>(r) * f.stride_uv, w);
    return true;
}

void CameraChannel::worker_loop() {
    std::shared_ptr<DmaBuffer> buf;
    while (running_.load(std::memory_order_relaxed)) {
        buf.reset();
        if (!q_ai_.pop(buf, std::chrono::milliseconds(100))) continue;
        if (sink_) sink_(buf);  // M3: → scheduler.submit_steady（构造 Job，帧引用随 Job 释放）
    }
    // 收尾排空：确保退出后 pool.in_use()==0（生命周期纪律）
    while (q_ai_.pop(buf, std::chrono::milliseconds(0))) {
        if (sink_) sink_(buf);
    }
}

void CameraChannel::watchdog_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(p_.watchdog_tick_ms));
        if (!running_.load(std::memory_order_relaxed)) break;
        State s = state_.load(std::memory_order_relaxed);
        if (s != State::RUNNING && s != State::LOADING) continue;

        int64_t last = last_frame_ms_.load(std::memory_order_relaxed);
        int64_t now = steady_ms();
        if (now - last > static_cast<int64_t>(p_.watchdog_timeout_ms)) {
            // 迁移表"看门狗超时"行：仅重启拉流，decoder/池/队列保留，状态不降级。
            // close() 强制 src 线程 read 失败 → 其重连路径完成实际重启（open 计数+1）；
            // 同时重置计时基准——给重连+首帧一个完整超时窗，否则旧基准会在
            // 重连宽限期内再次触发，形成连环重启（板上实测 opens=3 的根因）
            if (src_) src_->close();
            last_frame_ms_.store(now, std::memory_order_relaxed);
        }
    }
}

}  // namespace aibox
