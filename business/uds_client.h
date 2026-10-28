// UdsClient —— businessd→core-engine 客户端状态机（任务分解 T4.2，详设 §5.2）
//
// DISCONNECTED ──connect──► SYNCING ──全量 add_cam/set_algos ack──► ONLINE
//      ▲                        │ 部分失败：重试×2 后标记 degraded 继续          │
//      └── 断连/心跳超时 ◄───────┴───────────────────────────────────────────────┘
//
// SYNCING 序列（详设 §5.2/§5.7-5）：
//   1. hooks.ensure_proxies()：ZLM 代理重建（getMediaList 对比 DB，幂等）
//   2. 读 DB：cameras(enabled=1) + camera_algos
//   3. 逐台 add_cam → 等ack(5s, 重试×2, 失败=degraded 继续)；逐台 set_algos 同
//   4. 完成 → ONLINE；SYNCING 期间收到的 evt 一律丢弃（engine 重启旧事件无意义）
//
// ONLINE：
//   - 心跳 = get_metrics（engine ack ret=0；engine 端未知命令会 4001 断连，
//     故不用合成 ping 命令）；连续 3 次超时 → DISCONNECTED
//   - evt → hooks.on_evt（main 分发 WsHub/DB/outbox）；断线时 active_subs_
//     重连成功后自动重发 sub_meta（WsHub 订阅不因 engine 重启丢失）
//
// 因果序：add_cam/set_algos/remove_cam 等命令经 cmd_mu_ 全局串行（命令低频，
// 全局串行 = 最简单且严格强于"通道级保序"的实现，详设 §5.2 通道级因果序）
//
// 线程模型：单 client 线程（connect/sync/收包/心跳）；HttpApi 线程经 cmd_mu_ +
// send_mu_ 直发命令并在本线程等 ack（client 线程 pump 分发唤醒）。
// hooks.on_evt 在 client 线程回调——订阅方须自行保证线程安全。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "common/NonCopyable.h"
#include "db.h"
#include "ipc/Protocol.h"

namespace aibox {
namespace biz {

class UdsClient : public NonCopyable {
public:
    enum class State : uint8_t { DISCONNECTED, SYNCING, ONLINE };

    struct Hooks {
        // SYNCING 前调用（ZLM 代理重建，详设 §5.7-5）；false 仅告警不阻断同步
        std::function<bool()> ensure_proxies;
        // ONLINE 后的事件转发；SYNCING/DISCONNECTED 期事件不回调（已丢弃）。
        // line = 原始 JSON 行（detections 原样转发 WS 需要全字段透传，
        // 经 ParsedMsg 往返会丢未知字段）
        std::function<void(const proto::ParsedMsg& pm, const std::string& line)> on_evt;
    };

    struct SyncStats {  // 测试/诊断观测（last_sync 快照）
        int add_cam_ok = 0, add_cam_fail = 0;
        int set_algos_ok = 0, set_algos_fail = 0;
        int ensure_proxies_calls = 0;
        int degraded_cams = 0;  // 任一命令最终失败的通道数
    };

    struct Params {
        int connect_retry_ms = 1000;  // DISCONNECTED 重连间隔（详设 §5.2）
        int ack_timeout_ms = 5000;    // 单次等 ack 超时（详设 §5.2）
        int ack_retries = 2;          // SYNCING 失败重试次数（×2 后 degraded）
        int ping_interval_ms = 5000;  // ONLINE 心跳（get_metrics 口径）
        int ping_fail_limit = 3;      // 连续超时 → DISCONNECTED
    };

    // 默认参数用重载代替（嵌套类聚合的默认实参 = {} 在外层类完成前求值，GCC 拒绝；
    // 同 CameraChannel 先例）
    UdsClient(std::string sock_path, Db& db, Hooks hooks);
    UdsClient(std::string sock_path, Db& db, Hooks hooks, Params p);
    ~UdsClient();  // 自动 stop

    void start();
    void stop();

    State state() const { return state_.load(std::memory_order_relaxed); }
    bool online() const { return state() == State::ONLINE; }
    SyncStats sync_stats() const;

    // ---- 运行期命令（HttpApi/WsHub 调用；阻塞等 ack）----
    // 返回 engine ack；连接不可用/SYNCING 中/超时 → ret=-1、msg 描述原因
    // （state() 可进一步区分）。cmd_mu_ 保证因果序。
    proto::Ack add_cam(const proto::AddCam& v);
    proto::Ack set_algos(const proto::SetAlgos& v);
    proto::Ack remove_cam(const proto::RemoveCam& v);
    proto::Ack reload_algo(const proto::ReloadAlgo& v);
    bool get_metrics(std::string* data_out, int timeout_ms = 3000);  // data=ack.data

    // WsHub 引用计数联动（fire-and-forget：不等待 ack，不阻塞转发路径）。
    // 记入 active_subs_，重连后自动重发——engine 重启不丢订阅。
    bool sub_meta(uint32_t cam, bool subscribe);

private:
    void loop();              // 唯一 client 线程主循环
    bool connect_once();
    void close_sock();
    bool send_line(const std::string& line);   // send_mu_ 串行；MSG_NOSIGNAL 防崩
    bool pump(int timeout_ms);                 // poll+recv → 切行 → handle_line
    void handle_line(const std::string& line);
    bool do_sync();           // SYNCING 全量编排（client 线程内）
    bool wait_ack_here(uint64_t msg_id, proto::Ack& out, int timeout_ms);  // 本线程 pump 等
    // 发送前注册 pending（handle_line 的 ack 归属依赖此表——直接 send+wait 的
    // 路径若不先注册，engine 快 ack 会因查无此条被丢弃，SYNCING 永不完成，
    // 板上实测踩中）；disarm = 发送失败回收
    void arm_pending(uint64_t msg_id);
    void disarm_pending(uint64_t msg_id);
    proto::Ack request_for(uint64_t msg_id, const std::string& line,
                           int timeout_ms);  // 跨线程 cv 等
    void fail_all_pending(const char* reason);  // 断线时唤醒所有等待者
    uint64_t next_msg_id() { return msg_seq_.fetch_add(1, std::memory_order_relaxed); }
    void set_state(State s);

    const std::string sock_path_;
    Db& db_;
    Hooks hooks_;
    Params p_;

    std::atomic<State> state_{State::DISCONNECTED};
    std::atomic<int> sock_{-1};        // client 线程收发；stop() 跨线程 exchange(-1) 唤醒 poll
    std::string rbuf_;                 // client 线程独占

    std::mutex send_mu_;
    std::mutex cmd_mu_;                // 命令因果序（全局串行）

    std::mutex ack_mu_;
    std::condition_variable ack_cv_;
    struct Pending {
        bool done = false;
        bool is_ping = false;
        proto::Ack ack;
    };
    std::map<uint64_t, Pending> pending_;

    std::set<uint32_t> active_subs_;   // 已请求订阅的 cam（重连重发）

    std::atomic<uint64_t> msg_seq_{1};
    std::thread th_;
    std::atomic<bool> running_{false};
    SyncStats stats_;                  // mu_stats_ 保护
    mutable std::mutex mu_stats_;
};

}  // namespace biz
}  // namespace aibox
