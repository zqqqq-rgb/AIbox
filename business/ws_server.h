// WsServer —— 极简 RFC6455 WebSocket 服务端 + WsHub 订阅路由（任务分解 T4.5，详设 §5.5）
//
// 选型说明：cpp-httplib 不支持 WS 升级（官方 README: no websocket support），
// 故自写；WS 协议面窄（握手 + 文本/ping/pong/close 帧），线程模型复刻
// engine/UdsServer（单 epoll 线程独占收发 + 每连接发送队列 + 慢消费者丢最旧）。
// 浏览器发起 WS 连接不受 CORS 限制——独立端口对前端零负担。
//
// 路由（详设 §5.5）：
//   /ws/events        全局事件（camera_status/registry_ready/degrade/event…）广播
//   /ws/meta/{cam_id} detections 按通道组播；引用计数：首个 viewer 连接回调
//                     on_sub(cam)，全部断开回调 on_unsub(cam)（联动 UDS sub/unsub_meta）
//                     ——多 viewer 一路流只订一次（§4-T4.5 断言）
// 心跳：30s ping；连接 60s 无任何入站（pong/数据）→ 关闭（前端重连补拉）
// 背压：每连接发送队列 1024 帧，满丢最旧 + total_dropped 计数（慢消费者只丢自己的）
//
// 线程约定：send/broadcast 可从任意线程调用；on_sub/on_unsub 在 epoll 线程回调
// （订阅方须自行保证线程安全，如 UdsClient::sub_meta 已线程安全）。
// 帧支持：文本（服务端下发）/ping/pong/close；分片文本按累积重组（浏览器小消息
// 实际不分片，防御性支持）；二进制帧忽略。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/NonCopyable.h"

namespace aibox {
namespace biz {

class WsServer : public NonCopyable {
public:
    struct Hooks {
        std::function<void(uint32_t cam)> on_sub;    // cam 首个 viewer 连接
        std::function<void(uint32_t cam)> on_unsub;  // cam 最后一个 viewer 断开
    };

    explicit WsServer(Hooks hooks);
    ~WsServer();  // 自动 stop

    // 运行期补挂 hooks（main 装配顺序：ws 先于 uds 构造时，on_sub 需引用 uds——
    // 须在 start 前补挂完成；回调在 epoll 线程触发）
    void set_hooks(Hooks h) { hooks_ = std::move(h); }

    bool start(const std::string& host, int port);
    void stop();

    // 广播到 /ws/events 全部连接（线程安全；不阻塞）
    void broadcast(const std::string& text);
    // 组播到 /ws/meta/{cam} 订阅组（线程安全）
    void send_meta(uint32_t cam, const std::string& text);

    size_t conn_count() const;
    uint64_t total_dropped() const;

private:
    struct Conn {
        int fd = -1;
        uint32_t cam = 0;  // 0 = /ws/events（全局）；>0 = /ws/meta/{cam}
        std::string rx;    // 收包缓冲（握手期=HTTP，之后=WS 帧）
        bool ws_ready = false;
        uint8_t frag_opcode = 0;  // 分片重组：首帧 opcode（0=无分片进行中）
        std::string frag_payload;
        std::deque<std::string> txq;  // 已编码待发帧（epoll 线程刷出）
        int64_t last_rx_ms = 0;
        int64_t last_ping_ms = 0;
        bool out_armed = false;       // EPOLLOUT 挂载状态（防重复 MOD）
    };

    void loop();
    void accept_new();
    // 返回 false = 连接应关闭（握手失败/IO 错误/协议错误）
    bool handle_readable(Conn& c);
    bool try_handshake(Conn& c);          // HTTP Upgrade 握手（rx 攒到完整请求后）
    void on_ws_frame(Conn& c, uint8_t opcode, std::string payload);
    bool handle_writable(int fd);
    void close_conn(int fd);
    void enqueue_text(Conn& c, const std::string& text);   // 编码 + 背压
    void enqueue_frame(Conn& c, uint8_t opcode, const std::string& payload);
    void tick();                           // 心跳 ping / 超时清理
    void drop_sub(Conn& c);                // 从订阅组摘除（引用计数回调）

    Hooks hooks_;
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
