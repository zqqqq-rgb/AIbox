// UdsServer —— AF_UNIX SOCK_STREAM JSON-lines 服务端（任务分解 T2.5，详设 §3.10）
// 职责：'\n' 分帧收发、cmd 分发到 on_cmd 注册表、事件按订阅推送（架构 §5.3）、
//       慢消费者背压（每连接独立发送队列，满丢最旧）、空闲心跳
// 并发模型：单 epoll 线程独占全部 socket I/O（水平触发 LT——ET 需配非阻塞读尽，
//           LT 等价且更稳；性能足够（本地 UDS））；emit/on_cmd 可从任意线程调用
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "ipc/Protocol.h"

namespace aibox {

class UdsServer {
public:
    // 返回 Ack（ret/data 由处理器填，ack=msg_id 由 server 统一回填）；server 负责编码回发
    using CmdHandler = std::function<proto::Ack(const proto::ParsedMsg&)>;

    // idle_timeout_ms：空闲心跳——对端持续无入站流量超过该时长即关闭（0=禁用，测试用；
    // 默认 5000，详设 §3.10）。以默认参数兼作无参构造
    explicit UdsServer(uint32_t idle_timeout_ms = 5000);
    ~UdsServer();  // 自动 stop()

    UdsServer(const UdsServer&) = delete;
    UdsServer& operator=(const UdsServer&) = delete;

    // 监听 path（先 unlink 残留 socket 文件）；启动内部 epoll 线程；幂等（已启动返回 true）
    bool start(const std::string& path);

    // 关全部连接 + join 线程；幂等；stop 后 emit 安全（直接丢弃）
    void stop();

    // 命令处理器注册（main 装配时绑定各模块，详设 §3.11；需 start 前注册完）
    void on_cmd(const std::string& cmd, CmdHandler h);

    // 新客户端连入回调（架构 §5.3 全量重同步：engine 只在自身启动时广播一次
    // registry_ready，后连入的客户端永远收不到——main 在此补发全量状态）。
    // 回调在 epoll 线程、conn 已入表后触发；实现禁止重入 UdsServer 加锁接口
    // 以外的阻塞操作（emit 线程安全可直接调）
    void set_on_connect(std::function<void()> cb) { on_connect_ = std::move(cb); }

    // 事件推送（线程安全）：
    //   cam_id == 0 → 广播给所有连接
    //   cam_id >  0 → 仅发给已 sub_meta 订阅该 cam 的连接（detections 订阅制，架构 §5.3）
    // 每连接发送队列上限 1024 条，满丢最旧 + total_dropped 计数（详设 §3.10 背压），
    // 慢消费者只丢自己的帧，绝不阻塞 emit 调用方、不拖累其他连接
    void emit(const std::string& evt_json, uint32_t cam_id);

    size_t conn_count() const;
    uint64_t total_dropped() const;  // 全部连接慢消费者丢帧累计（监控用）

private:
    // 每连接会话状态（详设 §3.10：conn / subscriptions:set<cam_id> / last_ping）
    struct Conn {
        int fd = -1;
        std::string rbuf;             // 读缓冲，按 '\n' 分行（epoll 线程独占）
        std::deque<std::string> txq;  // 发送队列（mutex_ 保护：emit 入队 / epoll 线程刷出）
        std::set<uint32_t> subs;      // 已订阅 cam_id（mutex_ 保护：sub/unsub 写、emit 过滤读）
        int64_t last_active_ms = 0;   // 最近入站流量时刻（空闲心跳，epoll 线程独占）
        bool out_armed = false;       // EPOLLOUT 是否已挂（epoll 线程独占，避免重复 MOD）
        bool close_pending = false;   // 协议错误：回完 4001 ack 后关闭（详设 §3.10）
    };

    void loop();
    void accept_new();
    void handle_readable(int fd);
    void flush_conn(int fd);  // 仅 epoll 线程调用（socket 写必须单线程，防字节交错）
    void close_conn(int fd);  // 仅 epoll 线程调用
    void enqueue_locked(Conn& c, std::string&& line);  // 需已持 mutex_

    uint32_t idle_timeout_ms_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_dropped_{0};
    int listen_fd_ = -1;
    int epfd_ = -1;
    int wake_fds_[2] = {-1, -1};  // self-pipe：emit 入队后唤醒 epoll_wait
                                  // 留到析构才关闭（并发 emit 可能刚过检查正要写管道）
    std::string sock_path_;
    std::thread thread_;
    std::unordered_map<int, Conn> conns_;                    // mutex_ 保护
    std::unordered_map<std::string, CmdHandler> handlers_;   // mutex_ 保护（handler 锁外调用）
    std::function<void()> on_connect_;                       // 新连入回调（锁外触发）
};

}  // namespace aibox
