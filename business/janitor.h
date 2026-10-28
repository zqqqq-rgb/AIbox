// Janitor —— 磁盘水位循环删除 + Outbox 断网缓存重传（任务分解 T4.6，详设 §5.6/§5.8）
//
// DiskJanitor（降级⑤"磁盘水位→事件限流"在 businessd 侧的存储执行体）：
//   60s 巡检（详设 §5.6）：
//   1. 保留天数清理：segments 早于 keep_days → unlink 文件 + 删行（不受水位影响）
//   2. 水位兜底：statvfs 使用率 > 90% → 从最旧开始逐个删，直至 ≤90%
//   单轮 batch_max 上限（防一次巡检长阻塞 DB）；DB 本体 <50MB 不清理（无实现必要，
//   events/segments 行与 mp4 同源增长，删 segments 即控库）。
//   可测性：usage_fn 注入（测试返回 0.95 不用占满真实磁盘）；文件 unlink 为真实
//   系统调用（测试用临时目录小文件验证）。
//
// OutboxRetransmitter（详设 §5.8）：
//   IMqttPublisher 接缝——板上 libmosquitto 实现（QoS1 broker ack=confirmed），
//   测试注入假发布器（可编程成功/失败）。周期 drain：pending_outbox → publish →
//   ack → 删 outbox 行 + events.reported=1（对账字段）；失败保留待下轮（断网缓存）。
//   payload 约定：JSON 且可带 "_event_id" 字段（main 入队时附加），ack 后对账置位；
//   非事件类消息（心跳等）无该字段，仅删 outbox 行。
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "common/NonCopyable.h"
#include "db.h"

namespace aibox {
namespace biz {

class DiskJanitor : public NonCopyable {
public:
    struct Params {
        std::string mp4_dir;
        std::string snap_dir;        // 事件快照目录（空 = 不清理快照）
        int check_interval_s = 60;   // 详设 §5.6：60s 巡检
        double high_watermark = 0.90;
        int keep_days = 30;          // 0 = 不按天清理（仅水位兜底）
        int batch_max = 32;          // 单轮删除上限
    };

    // 返回 [0,1] 使用率；默认实现 statvfs；测试注入假值
    using UsageFn = std::function<double(const std::string& dir)>;

    DiskJanitor(Db& db, Params p, UsageFn usage_fn = {});
    ~DiskJanitor();

    void start();
    void stop();

    // 单轮清理（sweep 线程体亦复用此函数）：返回删除文件数
    int sweep_once();

private:
    int sweep_expired();
    int sweep_watermark(double usage);
    int sweep_snap_expired();  // 快照目录 mtime 早于 keep_days 的 jpg（与录像同生命周期）

    Db& db_;
    Params p_;
    UsageFn usage_fn_;
    std::thread th_;
    std::atomic<bool> running_{false};
};

// ---- MQTT 发布接缝 ----

class IMqttPublisher {
public:
    virtual ~IMqttPublisher() = default;
    // true = broker 已确认（QoS1 ack）；false = 暂不可用（保留重传）
    virtual bool publish(const std::string& topic, const std::string& payload) = 0;
};

class OutboxRetransmitter : public NonCopyable {
public:
    struct Params {
        int interval_ms = 1000;
        int batch = 64;
    };

    // 默认参数用重载代替（嵌套类聚合默认实参 = {} GCC 拒绝，同 CameraChannel 先例）
    OutboxRetransmitter(Db& db, IMqttPublisher* pub);
    OutboxRetransmitter(Db& db, IMqttPublisher* pub, Params p);
    ~OutboxRetransmitter();

    void start();
    void stop();

    // 单轮重传（线程体复用）：返回确认条数
    int drain_once();

private:
    Db& db_;
    IMqttPublisher* pub_;
    Params p_;
    std::thread th_;
    std::atomic<bool> running_{false};
};

}  // namespace biz
}  // namespace aibox
