// core-engine 装配入口（任务分解 T3.4，详设 §3.11 main 装配与生命周期）
//
// 双模式（同一二进制，--fake 切换）：
//   板上真件：RtspPuller + MppDecoder + DmaHeap 池 + RgaPreproc + RknnEngine
//   fake 链路：FileSource + FileDecoder + malloc 池 + SoftLetterbox + FakeInfer
//             （先用假件板上全链路验证骨架——16 通道 30min 稳定性；再逐步换真件）
//
// 启动序：signal → registry.load_all（坏包 disabled 不拖垮启动）→ scheduler/postproc
//         → UDS handlers → Monitor+DegradeController → registry_ready 广播 → 事件循环
// 优雅退出：g_shutdown → 全通道 stop（drain）→ scheduler stop → postproc 排空
//          → monitor stop → UDS stop。不持久化：重启白纸由 businessd 全量重建
//          （add_cam/set_algos 全量重放，架构 §5.3）
#include <signal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <json/json.h>

#include "arbiter/DegradeController.h"
#include "common/DmaBuffer.h"
#include "common/Log.h"
#include "common/NonCopyable.h"
#include "core/fakes/fake_infer.h"
#include "core/fakes/file_decoder.h"
#include "core/fakes/file_source.h"
#include "core/fakes/soft_letterbox.h"
#include "infer/NpuCorePlan.h"
#include "infer/NpuScheduler.h"
#include "infer/Yolov8Post.h"
#include "ipc/Protocol.h"
#include "ipc/UdsServer.h"
#include "media/CameraChannel.h"
#include "model/ModelRegistry.h"
#include "rules/RuleEngine.h"
#include "track/TrackerHub.h"

#include "hw/DmaHeapAllocator.h"
#include "hw/MppDecoder.h"
#include "hw/NpuMetricsSource.h"
#include "hw/RgaPreproc.h"
#include "hw/RknnEngine.h"
#include "hw/RtspPuller.h"

namespace aibox {
namespace {

std::atomic<bool> g_shutdown{false};
void on_sig(int) { g_shutdown.store(true); }

inline int64_t wall_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---- 命令行 ----
struct Args {
    bool fake = false;
    std::string packages = "/data/algorithms";
    std::string uds = "/tmp/aibox-engine.sock";
    std::string video;        // fake 模式：NV12 裸流文件（rtsp:// URL 之外可直传路径）
    uint32_t frame_w = 640, frame_h = 480;
    uint32_t fake_fps = 15;   // fake 源仿真帧率
    uint32_t connect_timeout_s = 5;
    std::string npu_cores;    // NpuCorePlan 显式覆盖（"steady=0,1:trig=2"）；空 = SoC auto
    // UDS 空闲心跳（0=禁用）。M3 默认 0：无 businessd 心跳时检测流是纯出站，
    // 5s 心跳会掐断测试连接；M4 businessd 接入后以 --uds-idle-ms 5000 打开（详设 §3.10）
    uint32_t uds_idle_ms = 0;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--fake") a.fake = true;
        else if (s == "--packages") a.packages = next();
        else if (s == "--uds") a.uds = next();
        else if (s == "--video") a.video = next();
        else if (s == "--frame-w") a.frame_w = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--frame-h") a.frame_h = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--fake-fps") a.fake_fps = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--connect-timeout")
            a.connect_timeout_s = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--npu-cores") a.npu_cores = next();
        else if (s == "--uds-idle-ms")
            a.uds_idle_ms = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "-h") {
            std::fprintf(stderr,
                         "用法: core-engine [--fake --video f.nv12 [--fake-fps N]]\n"
                         "                  [--packages DIR] [--uds PATH] "
                         "[--frame-w W --frame-h H]\n"
                         "                  [--npu-cores steady=0,1:trig=2]\n");
            std::exit(0);
        } else {
            LOGE("main", "未知参数: %s（-h 看用法）", s.c_str());
            std::exit(2);
        }
    }
    return a;
}

// ---- 后处理队列（MPSC：scheduler 两 worker 产出 → 单线程 tracker/rule/emit）----
// 单消费线程即够：16 通道 × 3fps = 48 帧/s 的跟踪+规则是微秒级。单线程内按到达序
// 串行执行，天然免除 TrackerHub/RuleEngine 的跨 worker 竞态（详设 §3.6 postproc 串行前提）
struct PostItem {
    uint32_t cam = 0;
    int64_t pts_us = 0;
    uint64_t frame_seq = 0;
    uint32_t frame_w = 0, frame_h = 0;  // 检测域分辨率（targets 坐标基准，透传 WS）
    std::vector<proto::DetTarget> targets;
};

class PostProc : public NonCopyable {
public:
    void push(PostItem&& it) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.size() >= 256) q_.pop_front();  // newest-wins（emit 慢不拖推理）
            q_.push_back(std::move(it));
        }
        cv_.notify_one();
    }
    // stop() 后仍排空剩余项再退出（生命周期纪律：已推理帧不丢）
    void run(const std::function<void(PostItem&)>& fn) {
        while (true) {
            PostItem it;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !q_.empty() || !running_; });
                if (q_.empty()) return;
                it = std::move(q_.front());
                q_.pop_front();
            }
            fn(it);
        }
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<PostItem> q_;
    bool running_ = true;
};

// fake 模式指标源：全零快照（sysfs 为板载概念；x86/骨架验证时降级判定恒不触发）
struct ZeroMetricsSource : IMetricsSource {
    MetricsSnapshot sample() override {
        MetricsSnapshot s;
        s.ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
        return s;
    }
};

// ---- 全局装配体 ----
struct App : NonCopyable {
    Args args;
    NpuCorePlan npu_plan;        // NPU 核分配（main 填充，UDS 启动前只读；get_metrics 曝光）
    DmaHeapAllocator dma_heap;   // 板上 AI 池分配器（fake 模式不使用）
    ModelRegistry registry;
    NpuScheduler sched;
    PostProc post;
    UdsServer uds;
    track::TrackerHub tracker;
    RuleEngine rules;
    DegradeController degrade;

    std::mutex mu;  // 保护 chans / chan_algos / algo_core 三个 map（冷路径 + 轻量读）
    std::map<uint32_t, std::unique_ptr<CameraChannel>> chans;
    std::map<uint32_t, std::vector<CameraChannel::AlgoCfg>> chan_algos;
    std::map<std::string, std::string> algo_core;  // algo_id → "steady"|"triggered"

    std::mutex met_mu;  // 保护 last_metrics（get_metrics 读）
    MetricsSnapshot last_metrics;

    App(Args a, ModelRegistry::RegistryDeps deps)
        : args(std::move(a)),
          registry(args.packages, std::move(deps)),
          uds(args.uds_idle_ms),
          degrade(make_levels(this)) {}

    // 降级 action 需引用自身 → 静态工厂在成员初始化期捕获 this
    //（lambda 运行期才解引用，届时成员全部构造完成）
    static std::vector<DegradeController::LevelCtl> make_levels(App* self);

    // registry 快照 → algo_core 表（调度桶选择用）
    void refresh_algo_core() {
        auto snap = registry.snapshot();  // 先落局部变量：成员访问临时对象会悬挂
        std::lock_guard<std::mutex> lk(mu);
        algo_core.clear();
        for (const auto& a : snap.algos)
            if (!a.disabled) algo_core[a.algo_id] = a.trigger;
    }
};

// 各 algo 权重 = Σ勾选通道 fps（DRR quantum，set_algos 全量重算语义）
void recompute_weights(App& app) {
    std::map<std::string, uint32_t> w;
    std::lock_guard<std::mutex> lk(app.mu);
    for (auto& kv : app.chan_algos)
        for (const auto& c : kv.second) w[c.algo_id] += c.detect_fps ? c.detect_fps : 1;
    for (auto& kv : w)
        if (auto inst = app.registry.get(kv.first)) inst->weight = kv.second;
}

// 降级①/④的 fps 目标：enter=全通道压到 1fps；恢复=回 set_algos 档（各 algo 最大 fps）
std::map<uint32_t, uint8_t> fps_plan(App* self, bool enter) {
    std::map<uint32_t, uint8_t> plan;
    std::lock_guard<std::mutex> lk(self->mu);
    for (auto& kv : self->chans) {
        uint8_t fps = 1;
        if (!enter) {
            auto it = self->chan_algos.find(kv.first);
            if (it != self->chan_algos.end())
                for (const auto& c : it->second)
                    if (c.detect_fps > fps) fps = c.detect_fps;
        }
        plan[kv.first] = fps;
    }
    return plan;
}

std::vector<DegradeController::LevelCtl> App::make_levels(App* self) {
    auto lv = DegradeController::default_levels();
    lv[0].action = [self](bool enter) {  // ① 常驻队列水位 → 逐通道降 fps
        for (auto& [id, fps] : fps_plan(self, enter)) self->sched.set_channel_fps(id, fps);
    };
    // ② DDR→VOP 抽帧：V1 预览抽帧在前端处理，engine 无 VOP，no-op
    lv[2].action = [self](bool enter) {  // ③ NPU 过载 → 触发推理暂停
        self->sched.set_triggered_paused(enter);
    };
    lv[3].action = [self](bool enter) {  // ④ 高温 → 全通道 fps=1（限频 sysfs V1 不做）
        for (auto& [id, fps] : fps_plan(self, enter)) self->sched.set_channel_fps(id, fps);
    };
    // ⑤ 磁盘水位：事件限流在 businessd 侧执行，engine no-op
    return lv;
}

// ---- sink：抽中的帧 → 每个勾选算法一个 Job（桶由 manifest.core 决定）----
std::function<void(std::shared_ptr<DmaBuffer>)> make_sink(App& app, uint32_t cam) {
    return [&app, cam](std::shared_ptr<DmaBuffer> buf) {
        std::vector<CameraChannel::AlgoCfg> algos;
        {
            std::lock_guard<std::mutex> lk(app.mu);
            auto it = app.chan_algos.find(cam);
            if (it != app.chan_algos.end()) algos = it->second;
        }
        // 源 pts 契约 = 归零毫秒（RtspPuller.Packet.pts_ms / FileSource 帧序×间隔），
        // sink 统一换算 us（详设 §12.5：Job.pts / detections.frame_pts 内部域为 us）。
        // ★ 板上实证：漏乘 1000 时 RuleEngine 的 ts 慢 1000 倍——DWELL 10s 实际
        // 要 2.8h；ByteTrack Kalman dt 同步失真（速度估计恒 ≈0）
        int64_t pts_us = buf->pts * 1000;
        for (const auto& c : algos) {
            auto inst = app.registry.get(c.algo_id);
            if (!inst) continue;  // reload 间隙/disabled：该算法本帧跳过
            Job j;
            j.cam_id = cam;
            j.pts = pts_us;
            j.frame_seq = buf->frame_seq;
            j.frame = buf;  // 拷贝引用（一帧多算法共用；释放随 Job 析构）
            j.conf_th = c.threshold;  // 通道级阈值覆盖（set_algos thr；<=0 用 manifest 值）
            j.algo = std::move(inst);
            j.pri = 3;  // 触发类目 V1 无分类配置，统一最低档（车牌/人脸分级 M4 接入）
            j.cb = [&app, cam, pts_us, seq = j.frame_seq, fw = buf->w, fh = buf->h](
                       std::vector<proto::DetTarget>&& ts) {
                PostItem it;
                it.cam = cam;
                it.pts_us = pts_us;
                it.frame_seq = seq;
                it.frame_w = fw;
                it.frame_h = fh;
                it.targets = std::move(ts);
                app.post.push(std::move(it));
            };
            bool triggered = false;
            {
                std::lock_guard<std::mutex> lk(app.mu);
                auto it = app.algo_core.find(c.algo_id);
                triggered = it != app.algo_core.end() && it->second == "triggered";
            }
            if (triggered)
                app.sched.submit_triggered(std::move(j));
            else
                app.sched.submit_steady(std::move(j));
        }
    };
}

// ---- 后处理单帧：tracker → rules → detections/rule events 推送 ----
void postproc_once(App& app, PostItem& it) {
    int64_t ts_ms = it.pts_us / 1000;
    app.tracker.update(it.cam, it.targets, ts_ms);
    // 归一化基准同步：RuleEngine 的 ROI 判定按"归一化多边形 × 实际解码帧"换算，
    // targets 坐标也来自该帧——每帧以解码实际尺寸为准（add_cam 时的 args 配置
    // 可能与相机子码流真实分辨率不符，错位会让锚点永远落在 ROI 外）
    app.rules.set_frame_size(it.cam, it.frame_w, it.frame_h);
    auto alarms = app.rules.eval(it.cam, it.targets, ts_ms);

    proto::DetectionsEvt det;
    det.id = it.cam;
    det.ts = wall_us();
    det.frame_seq = it.frame_seq;
    det.frame_pts = it.pts_us;
    det.frame_w = it.frame_w;
    det.frame_h = it.frame_h;
    det.targets = it.targets;
    app.uds.emit(proto::encode_event(det), it.cam);

    for (auto& al : alarms) {
        // 事件触发诊断（事件本身经防抖/单次武装节流，无频控必要）；
        // 带 track/框坐标——多目标时定位肇事框（板上排障：probe 只见 t[0] 稳定，
        // 实际是 t[1] 重复框在 ROI 边界振荡触发）
        if (!al.targets.empty()) {
            const auto& t0 = al.targets[0];
            LOGI("rules", "FIRE cam=%u algo=%s type=%s track=%d box=(%.0f,%.0f,%.0f,%.0f)",
                 al.cam, al.algo.c_str(), al.event_type.c_str(), t0.track_id,
                 t0.x, t0.y, t0.w, t0.h);
        }
        proto::RuleEventEvt ev;
        ev.id = al.cam;
        ev.algo = al.algo;
        ev.type = al.event_type;
        // ts 契约 = Unix ms（events 表/前端检索对齐录像）；al.ts_ms 是 pts 域
        // （防抖/驻留内部计时），出口必须换墙钟——否则落库时间落 1970，
        // 事件页按日期范围检索永远查不到
        ev.ts = wall_us() / 1000;
        ev.targets = std::move(al.targets);
        // 报警事件必须广播（cam_id=0）：emit 的订阅制语义是为 detections 高频流
        // 省带宽设计的；报警低频且关键——若按通道过滤，无人开宫格页（无
        // sub_meta 订阅）时事件在 engine 侧被静默丢弃，连 businessd 落库都到不了
        // （板上实证：只在宫格页才报警，切走即"不报警"）。camera_status 同理广播
        app.uds.emit(proto::encode_event(ev), 0);
    }
}

// ---- registry 工厂（fake / 板上真件）----
ModelRegistry::RegistryDeps fake_deps() {
    ModelRegistry::RegistryDeps d;
    d.make_preproc = [] { return std::static_pointer_cast<IPreproc>(std::make_shared<SoftLetterbox>()); };
    d.make_infer = [](const AlgoManifest& m, const std::string&,
                      std::string*) -> std::shared_ptr<IInferEngine> {
        Detection det;  // 稳定假框：通道画面中上部的 20% 框（16 通道冒烟可复现）
        det.label = 0;
        det.score = 0.9f;
        det.x = m.in_w * 0.25f;
        det.y = m.in_h * 0.25f;
        det.w = m.in_w * 0.2f;
        det.h = m.in_h * 0.2f;
        Targets t;
        t.boxes.push_back(det);
        return std::make_shared<FakeInfer>(std::vector<Targets>{t});
    };
    d.make_post = nullptr;  // FakeInfer 直出 [n,6] 原图坐标 → default_post
    return d;
}

ModelRegistry::RegistryDeps board_deps(const NpuCorePlan& plan) {
    // steady 组内按注册表构建序轮转分核（reload 重建时序号顺延、分配可能换核——可接受）；
    // triggered V1 固定首核（不并行，多核告警由 main 输出）
    auto steady_next = std::make_shared<std::atomic<uint32_t>>(0);
    ModelRegistry::RegistryDeps d;
    d.make_preproc = [] {
        return std::static_pointer_cast<IPreproc>(std::make_shared<RgaPreproc>());
    };
    d.make_infer = [plan, steady_next](const AlgoManifest& m, const std::string& pkg_dir,
                                       std::string* err) -> std::shared_ptr<IInferEngine> {
        auto eng = std::make_shared<RknnEngine>();
        auto fail = [&](const std::string& msg) {
            if (err) *err = msg;
            return std::shared_ptr<IInferEngine>();
        };
        if (!eng->load(pkg_dir + "/" + m.model_file))
            return fail("rknn load: " + eng->last_error());
        // K11：显式绑核（NpuCorePlan 分区：steady 组轮转 / triggered 首核），架构 §7.3
        int core_id = m.core == "triggered"
                          ? static_cast<int>(plan.triggered_cores[0])
                          : static_cast<int>(plan.steady_cores[steady_next->fetch_add(
                                                 1, std::memory_order_relaxed) %
                                             plan.steady_cores.size()]);
        if (!eng->set_core_index(core_id)) return fail("set_core: " + eng->last_error());
        // manifest × 模型 attr 交叉校验（防止 RGA 出 640 输入、模型要 320 的错位装配）
        if (m.in_w != eng->in_w() || m.in_h != eng->in_h()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "manifest 输入 %ux%u 与模型 attr %ux%u 不符",
                          m.in_w, m.in_h, eng->in_w(), eng->in_h());
            return fail(buf);
        }
        return eng;
    };
    d.make_post = [](const AlgoManifest& m, const IInferEngine& eng) -> PostFn {
        auto rk = dynamic_cast<const RknnEngine*>(&eng);
        if (!rk) return nullptr;
        YoloPostConfig cfg;
        cfg.num_cls = m.num_cls;
        cfg.input_w = m.in_w;
        cfg.input_h = m.in_h;
        cfg.conf_th = m.threshold > 0.f ? m.threshold : 0.45f;
        cfg.classes = m.classes;  // manifest 类白名单（漏带 = 全类输出，黄框根因）
        return make_yolov8_post(cfg, rk->out_hw());
    };
    return d;
}

// ---- 通道工厂（fake / 板上）----
CameraChannel::SourceFactory make_source_factory(App& app) {
    if (app.args.fake) {
        const uint32_t chunk = app.args.frame_w * app.args.frame_h * 3 / 2;  // NV12
        const uint32_t fps = app.args.fake_fps;
        const uint32_t interval_ms = fps ? 1000u / fps : 0u;  // 帧间隔 ms（fps=0 不限速）
        return [&app, chunk, interval_ms](const std::string& url) -> std::unique_ptr<ISource> {
            // rtsp:// 之外的 URL 视作文件路径（可按通道传不同测试文件）
            std::string path =
                url.rfind("rtsp://", 0) == 0 ? app.args.video : url;
            return std::make_unique<FileSource>(path, chunk, interval_ms, true /*loop*/);
        };
    }
    return [&app](const std::string& url) -> std::unique_ptr<ISource> {
        return std::make_unique<RtspPuller>(url, app.args.connect_timeout_s);
    };
}

CameraChannel::DecoderFactory make_decoder_factory(const App& app) {
    if (app.args.fake)
        return []() -> std::unique_ptr<IDecoder> { return std::make_unique<FileDecoder>(); };
    return []() -> std::unique_ptr<IDecoder> { return std::make_unique<MppDecoder>(); };
}

// ---- UDS cmd handlers（详设 §3.11 第 4 步；错误码 §8.2）----
void register_handlers(App& app) {
    auto src_factory = make_source_factory(app);
    auto dec_factory = make_decoder_factory(app);

    app.uds.on_cmd("add_cam", [&app, src_factory, dec_factory](
                                  const proto::ParsedMsg& m) -> proto::Ack {
        proto::Ack a;
        const auto& r = m.add_cam;
        // 查重前置：重复 id 不构造 Channel（构造后析构会走 stop 路径，徒增风险面）
        {
            std::lock_guard<std::mutex> lk(app.mu);
            if (app.chans.count(r.id)) {
                a.ret = 1003;
                a.msg = "cam already exists";
                return a;
            }
        }
        CameraChannel::Params p;
        p.frame_w = app.args.frame_w;
        p.frame_h = app.args.frame_h;
        p.pool_alloc = app.args.fake ? nullptr : &app.dma_heap;

        auto ch = std::make_unique<CameraChannel>(r.id, r.rtsp_sub, src_factory, dec_factory, p);
        ch->set_frame_sink(make_sink(app, r.id));
        {
            std::lock_guard<std::mutex> lk(app.mu);
            if (app.chans.count(r.id)) {
                a.ret = 1003;
                a.msg = "cam already exists";
                return a;
            }
            app.chans[r.id] = std::move(ch);  // 先入表再 start（start 起线程引用 this）
        }
        // start 在锁外（worker 首帧即可能调 sink → 取 app.mu，避免自旋等待）
        std::lock_guard<std::mutex> lk(app.mu);
        app.chans[r.id]->start();
        app.rules.set_frame_size(r.id, app.args.frame_w, app.args.frame_h);
        proto::CamStatusEvt ev;
        ev.id = r.id;
        ev.online = true;
        app.uds.emit(proto::encode_event(ev), 0);
        return a;
    });

    app.uds.on_cmd("remove_cam", [&app](const proto::ParsedMsg& m) -> proto::Ack {
        proto::Ack a;
        std::unique_ptr<CameraChannel> ch;
        {
            std::lock_guard<std::mutex> lk(app.mu);
            auto it = app.chans.find(m.remove_cam.id);
            if (it == app.chans.end()) {
                a.ret = 1002;
                a.msg = "cam not found";
                return a;
            }
            ch = std::move(it->second);
            app.chans.erase(it);
            app.chan_algos.erase(m.remove_cam.id);
        }
        ch->stop();  // 锁外：join 线程 + drain 队列（fd 归还池）
        ch.reset();  // 池析构（在途 buffer 必须已归还——stop 的生命周期纪律保证）
        app.rules.reset(m.remove_cam.id);
        app.tracker.reset(m.remove_cam.id);
        recompute_weights(app);
        proto::CamStatusEvt ev;
        ev.id = m.remove_cam.id;
        ev.online = false;
        app.uds.emit(proto::encode_event(ev), 0);
        return a;
    });

    app.uds.on_cmd("set_algos", [&app](const proto::ParsedMsg& m) -> proto::Ack {
        proto::Ack a;
        const auto& s = m.set_algos;
        std::vector<CameraChannel::AlgoCfg> cfgs;
        cfgs.reserve(s.algos.size());
        uint8_t max_fps = 1;
        for (const auto& sa : s.algos) {
            if (!app.registry.get(sa.algo)) {  // 未注册/disabled → 拒绝（1002）
                a.ret = 1002;
                a.msg = "algo not available: " + sa.algo;
                return a;
            }
            CameraChannel::AlgoCfg c;
            c.algo_id = sa.algo;
            c.threshold = sa.thr;
            c.detect_fps = sa.fps ? sa.fps : 1;
            c.roi = sa.roi;
            if (c.detect_fps > max_fps) max_fps = c.detect_fps;
            cfgs.push_back(std::move(c));
        }

        CameraChannel* ch = nullptr;
        {
            std::lock_guard<std::mutex> lk(app.mu);
            auto it = app.chans.find(s.id);
            if (it == app.chans.end()) {
                a.ret = 1002;
                a.msg = "cam not found";
                return a;
            }
            ch = it->second.get();
            app.chan_algos[s.id] = cfgs;  // 全量替换
        }
        ch->reconfigure(cfgs);  // 迁移表 RECONFIG（drain + 换表，同步 <500ms）
        app.sched.set_channel_fps(s.id, max_fps);
        recompute_weights(app);
        // ROI → 规则派生（事件接线，详设 §3.8）：set_algos 全量替换语义，
        // 规则表随通道配置全量重建（set_rules 内部清 track 状态，track_id 已重置）。
        // 未画 ROI 的算法仅检测不报警；多个算法各带 ROI → 各生成一条规则，
        // 防抖 key 含 event_type 互不干扰
        std::vector<RuleEngine::Rule> rrules;
        rrules.reserve(s.algos.size());
        for (const auto& sa : s.algos) {
            if (!sa.has_roi || sa.roi.size() < 3) continue;  // <3 点不构成多边形
            RuleEngine::Rule r;
            r.type = sa.rule_type == "dwell" ? RuleEngine::Type::DWELL
                                             : RuleEngine::Type::REGION_IN;
            r.polygon = sa.roi;
            r.event_type =
                sa.event_type.empty() ? sa.algo + "_" + sa.rule_type : sa.event_type;
            r.dwell_sec = sa.dwell_sec;
            r.debounce_sec = sa.debounce_sec;
            r.algo = sa.algo;
            rrules.push_back(std::move(r));
        }
        // 诊断（事件接线排障）：规则到达即打一条（配置动作低频，无频控必要）。
        // bbox 打印 ROI 归一化包围盒——与 probe 的 anchor 对照判"锚点是否在区域内"
        LOGI("rules", "cam=%u set_rules %zu 条", s.id, rrules.size());
        for (const auto& r : rrules) {
            float mnx = 1.f, mny = 1.f, mxx = 0.f, mxy = 0.f;
            for (const auto& p : r.polygon) {
                if (p.first < mnx) mnx = p.first;
                if (p.first > mxx) mxx = p.first;
                if (p.second < mny) mny = p.second;
                if (p.second > mxy) mxy = p.second;
            }
            LOGI("rules", "  algo=%s type=%s dwell=%us debounce=%us pts=%zu "
                          "roi_bbox=[%.2f,%.2f]-[%.2f,%.2f]",
                 r.algo.c_str(),
                 r.type == RuleEngine::Type::DWELL ? "dwell" : "region_in",
                 r.dwell_sec, r.debounce_sec, r.polygon.size(), mnx, mny, mxx, mxy);
        }
        app.rules.set_rules(s.id, std::move(rrules));
        return a;
    });

    app.uds.on_cmd("reload_algo", [&app](const proto::ParsedMsg& m) -> proto::Ack {
        proto::Ack a;
        if (!app.registry.reload(m.reload_algo.algo)) {
            a.ret = 3004;  // §8.2：自检失败已回滚
            a.msg = "reload failed (rollback attempted): " + m.reload_algo.algo;
        }
        app.refresh_algo_core();
        app.uds.emit(proto::encode_event(app.registry.snapshot()), 0);
        return a;
    });

    app.uds.on_cmd("get_metrics", [&app](const proto::ParsedMsg&) -> proto::Ack {
        proto::Ack a;
        MetricsSnapshot s;
        {
            std::lock_guard<std::mutex> lk(app.met_mu);
            s = app.last_metrics;
        }
        Json::Value d;
        d["npu"]["load_pct"] = s.npu_load_pct;
        // NpuCorePlan 分核曝光（board 装配后只读；对账 /sys/kernel/debug/rknpu/load：
        // 未分配核恒 0 = 分区纪律未被破坏的直接证据，架构 §7.7）
        Json::Value steady_cores(Json::arrayValue);
        for (uint8_t c : app.npu_plan.steady_cores) steady_cores.append(c);
        Json::Value trig_cores(Json::arrayValue);
        for (uint8_t c : app.npu_plan.triggered_cores) trig_cores.append(c);
        d["npu"]["steady_cores"] = steady_cores;
        d["npu"]["triggered_cores"] = trig_cores;
        d["ddr_pct"] = s.ddr_pct;
        d["temp_c"] = s.npu_temp_c;
        d["queue"]["steady_depth"] = static_cast<Json::UInt64>(app.sched.steady_depth());
        d["queue"]["triggered_depth"] =
            static_cast<Json::UInt64>(app.sched.triggered_depth());
        d["queue"]["uds_dropped"] = static_cast<Json::UInt64>(app.uds.total_dropped());
        Json::Value cams(Json::objectValue);
        std::lock_guard<std::mutex> lk(app.mu);
        for (auto& kv : app.chans) {
            auto& ch = *kv.second;
            Json::Value c;
            c["state"] = static_cast<int>(ch.state());
            c["src_opens"] = static_cast<Json::UInt64>(ch.source_opens());
            c["sampler_fps"] = ch.sampler().fps();
            auto put = [&c](const char* name, const EdgeMeter& mtr) {
                Json::Value e;
                e["in"] = static_cast<Json::UInt64>(mtr.in.load(std::memory_order_relaxed));
                e["out"] = static_cast<Json::UInt64>(mtr.out.load(std::memory_order_relaxed));
                e["drop"] = static_cast<Json::UInt64>(mtr.drop.load(std::memory_order_relaxed));
                e["sampled"] =
                    static_cast<Json::UInt64>(mtr.sampled.load(std::memory_order_relaxed));
                c[name] = e;
            };
            put("dec", ch.dec_meter());
            put("pool", ch.pool_meter());
            put("samp", ch.samp_meter());
            cams[std::to_string(kv.first)] = c;
        }
        d["cams"] = cams;
        Json::StreamWriterBuilder b;
        b["indentation"] = "";
        a.has_data = true;
        a.data = Json::writeString(b, d);
        return a;
    });
    // sub_meta / unsub_meta：UdsServer 会话订阅表内部处理（详设 §3.10），无 handler
}

}  // namespace
}  // namespace aibox

int main(int argc, char** argv) {
    using namespace aibox;
    Args a = parse_args(argc, argv);
    if (a.fake && a.video.empty()) {
        LOGE("main", "--fake 需要 --video <NV12 裸流文件>");
        return 2;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // NPU 核分配计划（NpuCorePlan）：--npu-cores 显式 > SoC auto（rk3588→[0,1]|[2]、
    // rk3576→[0]|[1]、未知→2 核兜底）；解析失败回退 auto 并告警，不拒启
    NpuCorePlan plan;
    std::string plan_note;
    if (!a.npu_cores.empty()) {
        if (!NpuCorePlan::parse(a.npu_cores, plan, &plan_note)) {
            LOGE("main", "--npu-cores 无效（%s），回退 auto",
                 plan_note.c_str());
            plan = NpuCorePlan::auto_detect(&plan_note);
        }
    } else {
        plan = NpuCorePlan::auto_detect(&plan_note);
    }
    auto cores_str = [](const std::vector<uint8_t>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += ',';
            s += std::to_string(v[i]);
        }
        return s;
    };
    LOGI("main", "npu plan: steady=[%s] triggered=[%s] %s",
         cores_str(plan.steady_cores).c_str(),
         cores_str(plan.triggered_cores).c_str(), plan_note.c_str());
    if (plan.triggered_n() > 1)
        LOGW("main", "npu plan: triggered %zu 核 V1 不并行，仅用首核 %d",
             plan.triggered_n(), static_cast<int>(plan.triggered_cores[0]));

    App app(a, a.fake ? fake_deps() : board_deps(plan));
    app.npu_plan = plan;

    // 1. 算法注册表（详设 §3.11 第 1~2 步：扫描 + 校验 + 自检；坏包 disabled）
    app.registry.load_all();
    app.refresh_algo_core();

    // 2. 降级①联动：set_channel_fps → 通道 Sampler（调度器不反向依赖通道）
    app.sched.set_fps_callback([&app](uint32_t cam, uint8_t fps) {
        std::lock_guard<std::mutex> lk(app.mu);
        auto it = app.chans.find(cam);
        if (it != app.chans.end()) it->second->sampler().configure(fps);
    });
    app.sched.start(app.npu_plan.steady_cores.size());  // worker 池 = steady 核数

    // 3. 后处理线程（tracker/rule/emit 串行消费）
    std::thread post_th([&app] {
        app.post.run([&app](PostItem& it) { postproc_once(app, it); });
    });

    // 4. UDS 命令面
    register_handlers(app);

    // 5. Monitor + DegradeController（500ms 快照驱动五级阶梯）
    std::shared_ptr<IMetricsSource> msrc =
        a.fake ? std::static_pointer_cast<IMetricsSource>(
                     std::make_shared<ZeroMetricsSource>())
               : std::static_pointer_cast<IMetricsSource>(
                     std::make_shared<NpuMetricsSource>());
    Monitor mon(msrc, 500);
    mon.set_on_snapshot([&app](const MetricsSnapshot& s) {
        MetricsSnapshot full = s;
        full.rq_steady_depth = app.sched.steady_depth();  // 降级①水位由 engine 侧补充
        {
            std::lock_guard<std::mutex> lk(app.met_mu);
            app.last_metrics = full;
        }
        app.degrade.on_snapshot(full);
    });
    app.degrade.set_on_change([&app](const proto::DegradeEvt& ev) {
        app.uds.emit(proto::encode_event(ev), 0);
    });
    mon.start();  // on_change 先注册再启动（避免首个降级事件漏发）

    // 6. UDS 服务 + registry_ready 广播
    // 新客户端连入即补发全量注册表（架构 §5.3 全量重同步——启动时的单次广播
    // 只覆盖当时已连接的客户端，businessd 后连入/重启会永远收不到 registry_ready）
    app.uds.set_on_connect([&app] {
        app.uds.emit(proto::encode_event(app.registry.snapshot()), 0);
    });
    if (!app.uds.start(app.args.uds)) {
        LOGE("main", "UDS listen fail: %s", app.args.uds.c_str());
        return 1;
    }
    app.uds.emit(proto::encode_event(app.registry.snapshot()), 0);
    LOGI("main", "core-engine up: mode=%s packages=%s uds=%s",
         app.args.fake ? "fake" : "board", app.args.packages.c_str(),
         app.args.uds.c_str());

    // 7. 事件循环（信号 → g_shutdown）
    while (!g_shutdown.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 8. 优雅退出（详设 §3.11 第 5 步；顺序：通道 → 调度 → 后处理 → 监控 → UDS）
    LOGI("main", "shutting down...");
    {
        std::vector<std::unique_ptr<CameraChannel>> to_stop;
        {
            std::lock_guard<std::mutex> lk(app.mu);
            for (auto& kv : app.chans) to_stop.push_back(std::move(kv.second));
            app.chans.clear();
            app.chan_algos.clear();
        }
        for (auto& ch : to_stop) ch->stop();  // 锁外 join（worker 的 sink 要取 mu）
    }
    app.sched.stop();   // join 双 worker（队列中 Job 析构，帧归还池）
    app.post.stop();    // 置停 + 排空剩余（run 循环退出条件）
    post_th.join();
    mon.stop();
    app.uds.stop();
    LOGI("main", "bye");
    return 0;
}
