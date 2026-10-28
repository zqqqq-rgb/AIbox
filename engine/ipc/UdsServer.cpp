#include "ipc/UdsServer.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <utility>
#include <vector>

namespace aibox {

namespace {

constexpr size_t kTxQueueCap = 1024;       // 详设 §3.10：每连接发送队列上限，满丢最旧
constexpr size_t kRbufMaxBytes = 1 << 20;  // 未换行读缓冲上限（防对端刷爆内存，按协议错误处理）

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

}  // namespace

UdsServer::UdsServer(uint32_t idle_timeout_ms) : idle_timeout_ms_(idle_timeout_ms) {}

UdsServer::~UdsServer() {
    stop();
    // wake 管道在此关闭（stop 不能关：并发 emit 可能刚过 running_ 检查正要写管道）
    if (wake_fds_[0] >= 0) ::close(wake_fds_[0]);
    if (wake_fds_[1] >= 0) ::close(wake_fds_[1]);
    wake_fds_[0] = wake_fds_[1] = -1;
}

bool UdsServer::start(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (running_.load(std::memory_order_relaxed)) return true;  // 幂等
    }
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return false;

    const int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) return false;
    ::unlink(path.c_str());  // 清残留 socket 文件（幂等启动的关键；不存在则忽略）

    sockaddr_un addr{};  // 零初始化，sun_path 天然以 '\0' 结尾
    addr.sun_family = AF_UNIX;
    path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    if (::bind(lfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(lfd, 16) < 0) {
        ::close(lfd);
        ::unlink(path.c_str());
        return false;
    }
    set_nonblock(lfd);

    const int ep = ::epoll_create1(0);
    if (ep < 0) {
        ::close(lfd);
        ::unlink(path.c_str());
        return false;
    }
    if (wake_fds_[0] < 0 && ::pipe(wake_fds_) < 0) {  // stop 后重启可复用旧管道
        ::close(ep);
        ::close(lfd);
        ::unlink(path.c_str());
        return false;
    }
    set_nonblock(wake_fds_[0]);
    set_nonblock(wake_fds_[1]);

    epoll_event ev{};  // 先赋值再挂（不使用指定初始化，C++17 保守写法）
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    ev.data.fd = wake_fds_[0];
    ::epoll_ctl(ep, EPOLL_CTL_ADD, wake_fds_[0], &ev);

    {
        std::lock_guard<std::mutex> lk(mutex_);
        sock_path_ = path;
        listen_fd_ = lfd;
        epfd_ = ep;
        running_.store(true, std::memory_order_relaxed);
    }
    thread_ = std::thread(&UdsServer::loop, this);
    return true;
}

void UdsServer::stop() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!running_.load(std::memory_order_relaxed)) return;  // 幂等
        running_.store(false, std::memory_order_relaxed);
    }
    if (thread_.joinable()) {
        const char b = 1;
        const ssize_t r = ::write(wake_fds_[1], &b, 1);  // 踢醒 epoll_wait 立即退出
        (void)r;                                         // 管道满时 100ms tick 兜底
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& kv : conns_) ::close(kv.first);  // 关全部连接
        conns_.clear();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
    sock_path_.clear();
}

void UdsServer::on_cmd(const std::string& cmd, CmdHandler h) {
    std::lock_guard<std::mutex> lk(mutex_);
    handlers_[cmd] = std::move(h);
}

void UdsServer::emit(const std::string& evt_json, uint32_t cam_id) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!running_.load(std::memory_order_relaxed)) return;  // stop 后安全：直接丢弃
        for (auto& kv : conns_) {
            Conn& c = kv.second;
            // 订阅制（架构 §5.3）：cam_id==0 广播；否则仅发已订阅该 cam 的连接
            if (cam_id != 0 && c.subs.find(cam_id) == c.subs.end()) continue;
            enqueue_locked(c, std::string(evt_json));  // 每连接一份拷贝，入队后 move
        }
    }
    if (wake_fds_[1] >= 0) {
        const char b = 1;
        const ssize_t r = ::write(wake_fds_[1], &b, 1);  // self-pipe 唤醒 epoll
        (void)r;                                         // 管道满忽略：100ms tick 兜底
    }
}

size_t UdsServer::conn_count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return conns_.size();
}

uint64_t UdsServer::total_dropped() const {
    return total_dropped_.load(std::memory_order_relaxed);
}

void UdsServer::enqueue_locked(Conn& c, std::string&& line) {
    // JSON-lines 出站帧定界：所有出站消息（ack/evt/4001）统一在此补 '\n'。
    // 缺此步对端按'\n'分帧将永远等不到完整行（板上实测抓出的 bug）
    if (line.empty() || line.back() != '\n') line.push_back('\n');
    // 背压（详设 §3.10）：满 1024 丢最旧并计数——慢消费者只丢自己的帧，
    // emit 调用方永不阻塞，其他连接不受拖累
    if (c.txq.size() >= kTxQueueCap) {
        c.txq.pop_front();
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    c.txq.push_back(std::move(line));
}

void UdsServer::loop() {
    std::vector<epoll_event> evs(64);
    while (running_.load(std::memory_order_relaxed)) {
        // 100ms 超时兼作空闲心跳检查的 tick；stop 靠 self-pipe 立即唤醒
        const int n = ::epoll_wait(epfd_, evs.data(), static_cast<int>(evs.size()), 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  // epoll 自身异常：退出线程，stop() 仍会完成清理
        }
        for (int i = 0; i < n; ++i) {
            const int fd = evs[i].data.fd;
            const uint32_t e = evs[i].events;
            if (fd == wake_fds_[0]) {
                // 排干 self-pipe（LT 下不读尽会持续唤醒）
                char sink[64];
                while (::read(wake_fds_[0], sink, sizeof(sink)) > 0) {
                }
                // emit 已在锁内入队：快照有积压的 fd 再逐个刷（避免 flush 中 map 失效）
                std::vector<int> dirty;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    for (auto& kv : conns_) {
                        if (!kv.second.txq.empty()) dirty.push_back(kv.first);
                    }
                }
                for (int cfd : dirty) flush_conn(cfd);
            } else if (fd == listen_fd_) {
                accept_new();
            } else {
                if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) handle_readable(fd);
                if (e & EPOLLOUT) flush_conn(fd);  // 与 IN 可同帧到达
            }
        }
        // 空闲心跳（详设 §3.10）：对端持续无入站流量即关闭；0=禁用
        if (idle_timeout_ms_ > 0) {
            const int64_t now = now_ms();
            std::vector<int> idle;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (auto& kv : conns_) {
                    if (now - kv.second.last_active_ms >
                        static_cast<int64_t>(idle_timeout_ms_)) {
                        idle.push_back(kv.first);
                    }
                }
            }
            for (int fd : idle) close_conn(fd);
        }
    }
}

void UdsServer::accept_new() {
    for (;;) {
        const int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            return;  // EAGAIN：暂无更多连接（listen_fd_ 非阻塞）
        }
        set_nonblock(cfd);  // accept 后全部非阻塞
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = cfd;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, cfd, &ev);  // 先挂 epoll 再入表（本线程串行，安全）
        {
            std::lock_guard<std::mutex> lk(mutex_);
            Conn c;
            c.fd = cfd;
            c.last_active_ms = now_ms();
            conns_.emplace(cfd, std::move(c));
        }
        // 锁外触发（回调可能 emit——emit 内部取 mutex_，锁内调用即自锁死锁）
        if (on_connect_) on_connect_();
    }
}

void UdsServer::handle_readable(int fd) {
    std::vector<std::string> lines;
    bool disconnect = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        Conn& c = it->second;
        if (c.close_pending) return;  // 已判协议错误：等 4001 发完即关，不再处理输入

        // 读到 EAGAIN（LT 下本轮未读尽也会再次上报，一次读尽只是减少唤醒）
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                c.rbuf.append(buf, static_cast<size_t>(n));
                c.last_active_ms = now_ms();  // 任何入站流量都喂狗（心跳基准=入站）
                if (c.rbuf.size() > kRbufMaxBytes) {
                    // 行未终止的超长输入：按协议错误处理，防对端刷爆内存
                    proto::Ack a;
                    a.ret = proto::kErrProtocol;
                    a.msg = "line too long";
                    enqueue_locked(c, proto::encode_ack(a));
                    c.close_pending = true;
                    break;
                }
                continue;
            }
            if (n == 0) {  // 对端关闭
                disconnect = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读尽
            disconnect = true;                                   // ECONNRESET 等真实 I/O 错误
            break;
        }
        if (disconnect) {
            conns_.erase(it);
            ::close(fd);  // close 自动从 epoll 注销
            return;
        }
        if (!c.close_pending) {  // 按 '\n' 切出完整行，残包留 rbuf
            size_t pos = 0;
            for (;;) {
                const size_t nl = c.rbuf.find('\n', pos);
                if (nl == std::string::npos) break;
                lines.emplace_back(c.rbuf, pos, nl - pos);
                pos = nl + 1;
            }
            c.rbuf.erase(0, pos);
        }
    }

    // 分发（绝不在锁内调 handler：handler 可能 emit/on_cmd，std::mutex 不可重入）
    for (std::string& line : lines) {
        proto::ParsedMsg pm;
        if (proto::decode_line(line, pm) != proto::kOk) {
            // 详设 §3.10：解析失败回 ERR_PROTOCOL 并断开；坏 JSON 无 msg_id → ack:0
            proto::Ack a;
            a.ack = pm.msg_id;  // 字段级错误时可回显已解出的 msg_id，完全坏 JSON 为 0
            a.ret = proto::kErrProtocol;
            a.msg = "protocol error";
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = conns_.find(fd);
            if (it == conns_.end()) return;
            enqueue_locked(it->second, proto::encode_ack(a));
            it->second.close_pending = true;  // flush 发完 4001 后关闭
            break;
        }
        if (pm.kind != "cmd") continue;  // engine 不收 evt/ack（详设 §8.1 单向约定），忽略

        // sub_meta/unsub_meta：会话级订阅由 server 内部处理（详设 §3.10/§3.11），
        // 不走 main 注册的 handler——直接回 ack(ret=0)，否则落空返回 1001 误导调用方
        if (pm.name == "sub_meta" || pm.name == "unsub_meta") {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = conns_.find(fd);
                if (it == conns_.end()) return;
                if (pm.name == "sub_meta") {
                    it->second.subs.insert(pm.sub_meta.id);
                } else {
                    it->second.subs.erase(pm.sub_meta.id);
                }
            }
            proto::Ack ack;
            ack.ack = pm.msg_id;  // 订阅变更成功即 ack；ret=0
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = conns_.find(fd);
            if (it == conns_.end()) return;
            enqueue_locked(it->second, proto::encode_ack(ack));
            continue;
        }

        CmdHandler h;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const auto hit = handlers_.find(pm.name);
            if (hit != handlers_.end()) h = hit->second;
        }
        proto::Ack ack;
        if (h) {
            ack = h(pm);  // ret/data 由处理器填
        } else {
            ack.ret = 1001;  // §8.2 参数错误域：未注册 handler（不断开）
            ack.msg = "no handler";
        }
        ack.ack = pm.msg_id;  // server 统一回填 msg_id 并编码回发
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = conns_.find(fd);
            if (it == conns_.end()) return;
            enqueue_locked(it->second, proto::encode_ack(ack));
        }
    }

    flush_conn(fd);  // 优先直接 write；EAGAIN 才挂 EPOLLOUT
}

void UdsServer::flush_conn(int fd) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Conn& c = it->second;
    bool dead = false;
    while (!c.txq.empty()) {
        std::string& front = c.txq.front();
        // MSG_NOSIGNAL：对端已关时得 EPIPE 而不是触发 SIGPIPE 杀进程
        const ssize_t n = ::send(fd, front.data(), front.size(), MSG_NOSIGNAL);
        if (n > 0) {
            if (static_cast<size_t>(n) < front.size()) {
                front.erase(0, static_cast<size_t>(n));  // 部分写：剩余留队等 EPOLLOUT
                break;
            }
            c.txq.pop_front();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // 发送缓冲满
        dead = true;  // EPIPE / ECONNRESET 等
        break;
    }
    if (dead) {
        conns_.erase(it);
        ::close(fd);
        return;
    }
    if (!c.txq.empty()) {
        if (!c.out_armed) {  // 队列非空 → 挂 EPOLLOUT
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLOUT;
            ev.data.fd = fd;
            ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
            c.out_armed = true;
        }
        return;
    }
    if (c.out_armed) {  // 队列已空 → 摘 EPOLLOUT，避免 LT 空转唤醒
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
        c.out_armed = false;
    }
    if (c.close_pending) {  // 协议错误的 4001 已发完 → 现在关闭（详设 §3.10）
        conns_.erase(it);
        ::close(fd);
    }
}

void UdsServer::close_conn(int fd) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (conns_.erase(fd) == 0) return;
    ::close(fd);  // close 自动从 epoll 注销
}

}  // namespace aibox
