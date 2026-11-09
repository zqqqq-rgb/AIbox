// WsServer —— 极简 RFC6455 WebSocket 服务端 + WsHub 订阅路由（任务分解 T4.5，详设 §5.5）
//
// 选型说明：cpp-httplib 不支持 WS 升级（官方 README: no websocket support），
// 故自写；WS 协议面窄（握手 + 文本/ping/pong/close 帧），线程模型复刻
// engine/UdsServer（单 epoll 线程独占收发 + 每连接有界发送队列）。
// 浏览器发起 WS 连接不受 CORS 限制——独立端口对前端零负担。
//
// 路由（详设 §5.5）：
//   /ws/events        全局事件（camera_status/registry_ready/degrade/event…）广播
//   /ws/meta/{cam_id} detections 按通道组播；引用计数：首个 viewer 连接回调
//                     on_sub(cam)，全部断开回调 on_unsub(cam)（联动 UDS sub/unsub_meta）
//                     ——多 viewer 一路流只订一次（§4-T4.5 断言）
// 心跳：30s ping；连接 60s 无任何入站（pong/数据）→ 关闭（前端重连补拉）
// 背压：同时限制帧数/字节；meta 只丢未开始的检测帧，events 超限关闭。
//
// 线程约定：send/broadcast 可从任意线程调用；on_sub/on_unsub 在 epoll 线程回调
// （订阅方须自行保证线程安全，如 UdsClient::sub_meta 已线程安全）。
// 帧支持：文本（服务端下发）/ping/pong/close；分片文本按累积重组（浏览器小消息
// 实际不分片，防御性支持）；二进制帧忽略。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

#include "common/NonCopyable.h"

namespace aibox {
namespace biz {

class AccessSession;

class WsServer : public NonCopyable {
public:
    struct Hooks {
        std::function<void(uint32_t cam)> on_sub;    // cam 首个 viewer 连接
        std::function<void(uint32_t cam)> on_unsub;  // cam 最后一个 viewer 断开
    };

    struct Limits {
        size_t max_tx_frames = 1024;
        size_t max_tx_bytes = 4 * 1024 * 1024;
        size_t max_frame_bytes = 1024 * 1024;
        size_t max_message_bytes = 1024 * 1024;
        size_t max_handshake_bytes = 8192;
        size_t max_connections = 128;
        int handshake_timeout_ms = 10000;
        int ping_interval_ms = 30000;
        int idle_timeout_ms = 60000;
    };
    struct Stats {
        uint64_t dropped_meta = 0, slow_connections = 0, rejected_connections = 0;
        uint64_t invalid_input = 0;
        size_t buffered_bytes = 0, buffered_frames = 0;
    };
    // Test transport seam: same contract as send(2); must not block or reenter WsServer.
    using Send = std::function<ssize_t(int, const void*, size_t, int)>;

    explicit WsServer(Hooks hooks);
    WsServer(Hooks hooks, Limits limits, Send send = {});
    ~WsServer();  // 自动 stop

    // 运行期补挂 hooks（main 装配顺序：ws 先于 uds 构造时，on_sub 需引用 uds——
    // 须在 start 前补挂完成；回调在 epoll 线程触发）
    void set_hooks(Hooks h) { hooks_ = std::move(h); }
    // Set before start; shared with HTTP so logout/expiry revokes live viewers.
    void set_access(std::shared_ptr<AccessSession> access) { access_ = std::move(access); }

    bool start(const std::string& host, int port);
    void stop();

    // 广播到 /ws/events 全部连接（线程安全；不阻塞）
    void broadcast(const std::string& text);
    // 组播到 /ws/meta/{cam} 订阅组（线程安全）
    void send_meta(uint32_t cam, const std::string& text);

    size_t conn_count() const;
    uint64_t total_dropped() const;
    Stats stats() const;

private:
    struct Conn {
        struct Frame { uint8_t opcode; std::string bytes; };
        int fd = -1;
        uint32_t cam = 0;  // 0 = /ws/events（全局）；>0 = /ws/meta/{cam}
        std::string rx;    // 收包缓冲（握手期=HTTP，之后=WS 帧）
        bool ws_ready = false;
        std::string session_id;
        bool bearer_access = false;
        uint8_t frag_opcode = 0;  // 分片重组：首帧 opcode（0=无分片进行中）
        std::string frag_payload;
        std::deque<Frame> txq;       // 尚未开始的帧；只有这里的检测帧可淘汰
        std::string active_frame;   // 包括握手响应；一旦开始，必须完整发完
        size_t active_offset = 0;
        size_t tx_bytes = 0;        // 包括完整 active allocation，完成后才释放预算
        bool close_requested = false;
        bool close_after_write = false;
        int64_t accepted_ms = 0;
        int64_t last_rx_ms = 0;
        int64_t last_ping_ms = 0;
        bool out_armed = false;       // EPOLLOUT 挂载状态（防重复 MOD）
    };

    void loop();
    void accept_new();
    // 返回 false = 连接应关闭（握手失败/IO 错误/协议错误）
    bool handle_readable(Conn& c);
    bool parse_input(Conn& c);
    bool try_handshake(Conn& c);          // HTTP Upgrade 握手（rx 攒到完整请求后）
    void on_ws_frame(Conn& c, uint8_t opcode, std::string payload);
    bool handle_writable(int fd);
    void close_conn(int fd);
    void enqueue_text(Conn& c, const std::string& text);   // 编码 + 背压
    void enqueue_frame(Conn& c, uint8_t opcode, const std::string& payload);
    void tick();                           // 心跳 ping / 超时清理
    void arm_output(Conn& c);
    void dispatch_hooks();                 // epoll 线程、锁外回调，允许回调重入查询
    void wake();

    Hooks hooks_;
    std::shared_ptr<AccessSession> access_;
    Limits limits_;
    Send send_;
    Stats counters_;
    std::vector<std::pair<uint32_t, bool>> hook_events_;
    int listen_fd_ = -1;
    int epfd_ = -1;
    int wake_fds_[2] = {-1, -1};  // self-pipe：enqueue 后唤醒 epoll_wait
    std::string host_;
    int port_ = 0;

    mutable std::mutex mu_;       // 保护 conns_/subs_（send 跨线程 + const conn_count）
    std::unordered_map<int, Conn> conns_;
    std::map<uint32_t, std::set<int>> meta_subs_;  // cam → fds（引用计数）

    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_dropped_{0};
};

}  // namespace biz
}  // namespace aibox
