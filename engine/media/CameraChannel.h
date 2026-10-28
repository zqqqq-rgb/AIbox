// CameraChannel —— 通道粒度故障域（任务分解 T2.2，详设 §3.1，架构 §9 第 1 层）
//
// 状态机迁移表（详设 §3.1）：
//   INIT     | start()             | 建池/decoder，开源，起线程           | LOADING
//   LOADING  | 首帧到达             | ——                                  | RUNNING
//   RUNNING  | reconfigure()        | drain q + sampler 令牌 + 换算法表    | RECONFIG→RUNNING
//   RUNNING  | 看门狗超时           | 仅重启源（decoder/池/队列保留）      | RUNNING
//   任意     | stop()               | 停源→drain→join                     | STOPPED
//
// 看门狗语义（M2 落地形态）：超时只 close() 源；重连是 src 线程唯一职责
//（read 失败 → backoff → open()）。板上 RtspPuller 的 restart() 封装同一时序，
// MPP ctx / 池 / 队列全部保留——SPS 不变时解码器无需重建（详设 §3.1 看门狗细节）。
//
// M2 x86 注入点：SourceFactory / DecoderFactory（板上 main 装配换成 RtspPuller/MppDecoder）；
// 下游消费经 set_frame_sink（M3 main 装配 → NpuScheduler.submit_steady）
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/BoundedQueue.h"
#include "common/DmaBuffer.h"
#include "common/EdgeMeter.h"
#include "common/FramePool.h"
#include "common/NonCopyable.h"
#include "interfaces/idecoder.h"
#include "interfaces/isource.h"
#include "media/Sampler.h"

namespace aibox {

class CameraChannel : public NonCopyable {
public:
    enum class State : uint8_t { INIT, LOADING, RUNNING, RECONFIG, STOPPED };

    // 通道侧算法配置（proto::AlgoCfg 的镜像；set_algos 全量替换语义）
    struct AlgoCfg {
        std::string algo_id;
        float threshold = 0.5f;
        uint8_t detect_fps = 3;
        std::vector<std::pair<float, float>> roi;  // 归一化多边形，空=全屏
    };

    using SourceFactory = std::function<std::unique_ptr<ISource>(const std::string& url)>;
    using DecoderFactory = std::function<std::unique_ptr<IDecoder>()>;

    struct Params {
        uint32_t pool_depth = 8;  // AI 池深 = RGA+NPU 在途上限（详设 §3.1）
        uint32_t frame_w = 640, frame_h = 480;
        uint32_t watchdog_tick_ms = 1000;    // 板上默认；测试注入短值
        uint32_t watchdog_timeout_ms = 5000;
        uint32_t reconnect_backoff_ms = 200;  // read 失败后的重连间隔（单层重试）
        // 板上注入 DmaHeapAllocator（dma-buf 真池）；空 = x86 内置 malloc 模拟池
        IBufferAllocator* pool_alloc = nullptr;
    };

    // 默认参数用重载代替（嵌套类 NSDMI 在外层类完成前的默认实参求值，GCC 拒绝）
    CameraChannel(uint32_t cam_id, std::string rtsp_url, SourceFactory mk_src,
                  DecoderFactory mk_dec);
    CameraChannel(uint32_t cam_id, std::string rtsp_url, SourceFactory mk_src,
                  DecoderFactory mk_dec, Params p);
    ~CameraChannel();  // 未 stop 则自动 stop

    void start();  // INIT→LOADING
    // RUNNING→RECONFIG→RUNNING，同步完成（<500ms 验收：drain + 换表，无 I/O）
    void reconfigure(const std::vector<AlgoCfg>& algos);
    void stop();  // →STOPPED

    State state() const { return state_.load(std::memory_order_relaxed); }

    void set_frame_sink(std::function<void(std::shared_ptr<DmaBuffer>)> sink) {
        sink_ = std::move(sink);
    }

    // 监控面（Monitor 差分 / get_metrics）
    const EdgeMeter& dec_meter() const { return m_dec_; }    // 解码出帧边
    const EdgeMeter& pool_meter() const { return m_pool_; }  // 池获取边（drop=池空）
    const EdgeMeter& samp_meter() const { return m_samp_; }  // 抽帧边（sampled=令牌拒）
    const BoundedQueue<std::shared_ptr<DmaBuffer>, 4>& q_ai() const { return q_ai_; }
    Sampler& sampler() { return sampler_; }              // 降级联动（fps_cb 经 main 装配）
    const Sampler& sampler() const { return sampler_; }
    uint64_t source_opens() const { return source_opens_.load(std::memory_order_relaxed); }

private:
    void src_loop();
    void worker_loop();
    void watchdog_loop();
    void set_state(State s);
    bool copy_into_pool(const Frame& f, const std::shared_ptr<DmaBuffer>& buf);

    const uint32_t cam_id_;
    const std::string rtsp_url_;
    SourceFactory mk_src_;
    DecoderFactory mk_dec_;
    const Params p_;

    std::unique_ptr<ISource> src_;
    std::unique_ptr<IDecoder> dec_;
    FramePool pool_;
    BoundedQueue<std::shared_ptr<DmaBuffer>, 4> q_ai_;
    Sampler sampler_{3};

    std::function<void(std::shared_ptr<DmaBuffer>)> sink_;
    mutable std::mutex algos_mu_;
    std::vector<AlgoCfg> algos_;  // M3 main 装配消费（M2 通道只存储）

    std::atomic<State> state_{State::INIT};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> last_frame_ms_{0};   // 看门狗基准（解码成功即刷新）
    std::atomic<uint64_t> source_opens_{0};   // open() 累计（看门狗"恰好 1 次"断言）
    std::atomic<uint64_t> frame_seq_{0};      // 抽帧计数（仅采样通过者）
    std::atomic<bool> pool_warn_once_{false}; // 池边拒绝首帧告警（尺寸/池耗尽二分，防刷屏）

    EdgeMeter m_dec_, m_pool_, m_samp_;
    std::thread src_th_, worker_th_, wd_th_;
};

}  // namespace aibox
