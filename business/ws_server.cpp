#include "ws_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "common/Log.h"

namespace aibox {
namespace biz {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// ---- SHA1（RFC 3174 紧凑实现，仅用于 WS 握手，无安全用途）----
inline uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> msg(data, data + len);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const size_t p = off + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(msg[p]) << 24) |
                   (static_cast<uint32_t>(msg[p + 1]) << 16) |
                   (static_cast<uint32_t>(msg[p + 2]) << 8) |
                   static_cast<uint32_t>(msg[p + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(p[i]) << 16) |
                           (i + 1 < n ? static_cast<uint32_t>(p[i + 1]) << 8 : 0) |
                           (i + 2 < n ? static_cast<uint32_t>(p[i + 2]) : 0);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tbl[v & 63] : '=');
    }
    return out;
}

// ---- WS 帧编解码 ----
// 服务端下发不掩码；客户端上行必须掩码（RFC 6455 §5.1）
std::string encode_frame(uint8_t opcode, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));  // fin=1
    const size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF));
    }
    out += payload;
    return out;
}

constexpr size_t kMaxFrame = 1 << 20;  // 1MB：detections 消息远小于此

}  // namespace

WsServer::WsServer(Hooks hooks) : hooks_(std::move(hooks)) {}

WsServer::~WsServer() { stop(); }

bool WsServer::start(const std::string& host, int port) {
    if (running_.exchange(true)) return true;  // 幂等
    host_ = host;
    port_ = port;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        running_ = false;
        return false;
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr =
        host.empty() || host == "0.0.0.0" ? INADDR_ANY : inet_addr(host.c_str());
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 16) != 0) {
        LOGE("ws", "bind %s:%d 失败: %s", host.c_str(), port, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return false;
    }
    set_nonblock(listen_fd_);

    epfd_ = ::epoll_create1(0);
    wake_fds_[0] = wake_fds_[1] = -1;
    if (epfd_ < 0 || ::pipe(wake_fds_) != 0) {
        stop();
        running_ = false;
        return false;
    }
    set_nonblock(wake_fds_[0]);
    set_nonblock(wake_fds_[1]);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev);
    ev.data.fd = wake_fds_[0];
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, wake_fds_[0], &ev);

    th_ = std::thread([this] { loop(); });
    LOGI("ws", "WsServer listening %s:%d", host.c_str(), port);
    return true;
}

void WsServer::stop() {
    if (!running_.exchange(false)) return;
    if (wake_fds_[1] >= 0) {
        const char c = 'w';
        ssize_t r = 0;
        do {
            r = ::write(wake_fds_[1], &c, 1);
        } while (r < 0 && errno == EINTR);
    }
    if (th_.joinable()) th_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epfd_ >= 0) ::close(epfd_);
    if (wake_fds_[0] >= 0) ::close(wake_fds_[0]);
    if (wake_fds_[1] >= 0) ::close(wake_fds_[1]);
    listen_fd_ = epfd_ = wake_fds_[0] = wake_fds_[1] = -1;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : conns_) ::close(kv.first);
    conns_.clear();
    meta_subs_.clear();
}

size_t WsServer::conn_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return conns_.size();
}

uint64_t WsServer::total_dropped() const { return total_dropped_.load(); }

// ---- 发送面（跨线程）----

void WsServer::broadcast(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : conns_) {
        Conn& c = kv.second;
        if (c.ws_ready && c.cam == 0) enqueue_text(c, text);
    }
}

void WsServer::send_meta(uint32_t cam, const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = meta_subs_.find(cam);
    if (it == meta_subs_.end()) return;
    for (const int fd : it->second) {
        const auto cit = conns_.find(fd);
        if (cit != conns_.end()) enqueue_text(cit->second, text);
    }
}

// 需已持 mu_
void WsServer::enqueue_text(Conn& c, const std::string& text) {
    enqueue_frame(c, 0x1, text);
}

// 需已持 mu_；满丢最旧（慢消费者只丢自己的，详设 §5.5 背压纪律）
void WsServer::enqueue_frame(Conn& c, uint8_t opcode, const std::string& payload) {
    if (c.txq.size() >= 1024) {
        c.txq.pop_front();
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    c.txq.push_back(encode_frame(opcode, payload));
    if (!c.out_armed) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = c.fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &ev);
        c.out_armed = true;
    }
}

// ---- 连接管理 ----

void WsServer::accept_new() {
    // 需已持 mu_（loop 独占锁内调用）——不可再加锁：std::mutex 非递归，
    // 二次 lock 自锁死锁（板上实测踩中：epoll 线程卡死于此，连接进不了 epoll）
    for (;;) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) break;
        set_nonblock(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Conn c;
        c.fd = fd;
        c.last_rx_ms = now_ms();
        conns_[fd] = std::move(c);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }
}

void WsServer::drop_sub(Conn& c) {
    // 需已持 mu_；on_unsub 由调用方延迟触发（锁外回调纪律）
    if (c.cam == 0) return;
    const auto it = meta_subs_.find(c.cam);
    if (it == meta_subs_.end()) return;
    it->second.erase(c.fd);
    if (it->second.empty()) meta_subs_.erase(it);
}

void WsServer::close_conn(int fd) {
    // 需已持 mu_；返回值 = 是否需要回调 on_unsub（锁外触发）
    const auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Conn& c = it->second;
    const uint32_t cam = c.cam;
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    conns_.erase(it);
    if (cam != 0) {
        const auto sit = meta_subs_.find(cam);
        if (sit != meta_subs_.end()) {
            sit->second.erase(fd);
            if (sit->second.empty()) {
                meta_subs_.erase(sit);
                if (hooks_.on_unsub) hooks_.on_unsub(cam);
            }
        }
    }
}

// ---- 握手 ----

bool WsServer::try_handshake(Conn& c) {
    const size_t hdr_end = c.rx.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        if (c.rx.size() > 8192) return false;  // 异常握手请求
        return true;                           // 继续攒
    }
    // 请求行 + 头部提取（大小写不敏感）
    std::string path;
    std::string key;
    size_t pos = 0;
    bool first = true;
    while (pos < hdr_end) {
        const size_t line_end = c.rx.find("\r\n", pos);
        const size_t stop = line_end == std::string::npos ? hdr_end : line_end;
        const std::string line = c.rx.substr(pos, stop - pos);
        if (first) {
            first = false;
            // "GET /ws/meta/3 HTTP/1.1"
            const size_t sp1 = line.find(' ');
            const size_t sp2 = line.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos)
                path = line.substr(sp1 + 1, sp2 - sp1 - 1);
        } else {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                for (char& ch : name) ch = static_cast<char>(::tolower(ch));
                if (name == "sec-websocket-key") {
                    key = line.substr(colon + 1);
                    while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
                        key.erase(key.begin());
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                        key.pop_back();
                }
            }
        }
        if (line_end == std::string::npos) break;
        pos = line_end + 2;
    }
    c.rx.erase(0, hdr_end + 4);

    // 路由：/ws/events → cam=0；/ws/meta/{id} → cam=id
    uint32_t cam = 0;
    if (path == "/ws/events") {
        cam = 0;
    } else if (path.rfind("/ws/meta/", 0) == 0) {
        cam = static_cast<uint32_t>(std::atoi(path.c_str() + 9));
        if (cam == 0) return false;
    } else {
        return false;
    }
    if (key.empty()) return false;

    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string accept_src = key + kGuid;
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(accept_src.data()), accept_src.size(), digest);
    const std::string accept = base64(digest, 20);

    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " +
                       accept + "\r\n\r\n";
    size_t off = 0;
    while (off < resp.size()) {
        const ssize_t n = ::send(c.fd, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }

    c.ws_ready = true;
    c.cam = cam;
    if (cam != 0) {
        const bool first_viewer = meta_subs_[cam].empty();
        meta_subs_[cam].insert(c.fd);
        if (first_viewer && hooks_.on_sub) {
            const uint32_t cam_id = cam;
            hooks_.on_sub(cam_id);  // 锁内调用——on_sub 实现须避免重入 WsServer
        }
    }
    LOGI("ws", "viewer connected: %s (cam=%u)", path.c_str(), cam);
    return true;
}

// ---- 帧处理 ----

void WsServer::on_ws_frame(Conn& c, uint8_t opcode, std::string payload) {
    switch (opcode) {
        case 0x8:  // close
            enqueue_frame(c, 0x8, "");
            break;
        case 0x9:  // ping → pong
            enqueue_frame(c, 0xA, payload);
            break;
        case 0xA:  // pong：last_rx 已由收包刷新
            break;
        case 0x1:  // 文本：客户端正常不发业务消息；忽略内容（心跳/占位）
        case 0x2:  // 二进制忽略
        default:
            break;
    }
}

// 返回 false = 关闭连接；需已持 mu_（loop 独占调用）
bool WsServer::handle_writable(int fd) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) return true;
    Conn& c = it->second;
    while (!c.txq.empty()) {
        std::string& front = c.txq.front();
        const ssize_t n = ::send(fd, front.data(), front.size(), MSG_NOSIGNAL);
        if (n > 0) {
            front.erase(0, static_cast<size_t>(n));
            if (front.empty()) c.txq.pop_front();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // 余量待 EPOLLOUT
        return false;  // EPIPE 等
    }
    return true;
}

bool WsServer::handle_readable(Conn& c) {
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.rx.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }
    if (!c.ws_ready) {
        if (!try_handshake(c)) return false;
        if (!c.ws_ready) return true;  // 握手请求未攒全
        // 握手完成：若同一批 recv 带来了后续帧，直接进入帧解析
    }

    // 帧解析（增量）
    for (;;) {
        if (c.rx.size() < 2) break;
        const auto* p = reinterpret_cast<const uint8_t*>(c.rx.data());
        const bool fin = (p[0] & 0x80) != 0;
        const uint8_t opcode = p[0] & 0x0F;
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t off = 2;
        if (len == 126) {
            if (c.rx.size() < off + 2) break;
            len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
            off += 2;
        } else if (len == 127) {
            if (c.rx.size() < off + 8) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[off + i];
            off += 8;
        }
        if (len > kMaxFrame) return false;  // 异常大帧：断开
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (c.rx.size() < off + 4) break;
            std::memcpy(mask, p + off, 4);
            off += 4;
        } else if (opcode != 0x0 && opcode < 0x8) {
            // 客户端数据帧必须掩码（控制帧/分片宽容，浏览器恒掩码）
            return false;
        }
        if (c.rx.size() < off + len) break;  // 不完整，继续攒
        std::string payload = c.rx.substr(off, static_cast<size_t>(len));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        c.rx.erase(0, static_cast<size_t>(off + len));

        if (opcode >= 0x8) {  // 控制帧：不分片
            on_ws_frame(c, opcode, std::move(payload));
            if (opcode == 0x8) return false;  // close：回帧后断开
            continue;
        }
        // 数据帧分片重组
        if (opcode != 0x0) {
            if (fin) {
                on_ws_frame(c, opcode, std::move(payload));
            } else {
                c.frag_opcode = opcode;
                c.frag_payload = std::move(payload);
            }
        } else {
            c.frag_payload += payload;
            if (fin && c.frag_opcode != 0) {
                const uint8_t op = c.frag_opcode;
                c.frag_opcode = 0;
                on_ws_frame(c, op, std::move(c.frag_payload));
                c.frag_payload.clear();
            }
        }
    }
    return true;
}

// ---- 主循环 ----

void WsServer::tick() {
    // 需已持 mu_；30s ping / 60s 无入站断开
    const int64_t now = now_ms();
    std::vector<int> to_close;
    for (auto& kv : conns_) {
        Conn& c = kv.second;
        if (!c.ws_ready) {
            if (now - c.last_rx_ms > 10000) to_close.push_back(c.fd);  // 握手超时
            continue;
        }
        if (now - c.last_rx_ms > 60000) {
            to_close.push_back(c.fd);
            continue;
        }
        if (now - c.last_ping_ms >= 30000) {
            c.last_ping_ms = now;
            enqueue_frame(c, 0x9, "");
        }
    }
    for (const int fd : to_close) close_conn(fd);
}

void WsServer::loop() {
    int64_t last_tick = 0;
    while (running_.load()) {
        epoll_event evs[64];
        const int n = ::epoll_wait(epfd_, evs, 64, 200);
        if (n < 0 && errno != EINTR) break;

        // mu_ 全程独占：收/发/连接表操作都很快，且发送方（HttpApi 线程）只做入队
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < n; ++i) {
            const int fd = evs[i].data.fd;
            if (fd == listen_fd_) {
                accept_new();
                continue;
            }
            if (fd == wake_fds_[0]) {  // 唤醒：读尽
                char b[64];
                while (::read(fd, b, sizeof(b)) > 0) {}
                continue;
            }
            {
                const uint32_t rev = evs[i].events;
                if (rev & (EPOLLHUP | EPOLLERR)) {
                    close_conn(fd);
                    continue;
                }
                // 先写后读（写完可能读继续；各自重新 find，防 erase 后悬垂）
                if (rev & EPOLLOUT) {
                    if (!handle_writable(fd)) {
                        close_conn(fd);
                        continue;
                    }
                }
                if (rev & EPOLLIN) {
                    auto it = conns_.find(fd);
                    if (it == conns_.end()) continue;
                    it->second.last_rx_ms = now_ms();
                    if (!handle_readable(it->second)) {
                        close_conn(fd);
                        continue;
                    }
                }
            }
            // 发送队列状态同步 EPOLLOUT 挂载（编码入队后唤醒缺失的兜底）
            const auto it = conns_.find(fd);
            if (it == conns_.end()) continue;
            Conn& c = it->second;
            const bool want_out = !c.txq.empty();
            if (want_out != c.out_armed) {
                epoll_event cev{};
                cev.events = EPOLLIN;
                if (want_out) cev.events |= EPOLLOUT;  // 枚举/非枚举三元不做混算（-Wextra）
                cev.data.fd = fd;
                ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &cev);
                c.out_armed = want_out;
            }
        }
        if (now_ms() - last_tick >= 1000) {
            last_tick = now_ms();
            tick();
        }
    }
    // 退出时清连接（stop() 也会清，兜底防止 fd 泄漏）
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : conns_) ::close(kv.first);
    conns_.clear();
    meta_subs_.clear();
}

}  // namespace biz
}  // namespace aibox
