// MockZlm —— 进程内极简 ZLM REST mock（T4.3/T4.4 测试共用）
// 单线程 accept + per-conn 一次性请求应答；按路径脚本化应答，计数器供断言
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aibox {
namespace testutil {

inline std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(),
                                                        nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

class MockZlm {
public:
    std::atomic<bool> fail_sub_add{false};  // addStreamProxy _sub 恒失败
    std::atomic<int> add_main{0};
    std::atomic<int> add_sub{0};
    std::atomic<int> del{0};

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 临时端口
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            return false;
        port_ = ntohs(addr.sin_port);
        if (::listen(listen_fd_, 8) != 0) return false;
        running_.store(true);
        th_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        // 注意：不能依赖 close 唤醒阻塞的 accept（Linux 不保证）——accept_loop
        // 以 50ms poll 轮询 running_，join 必然在半秒内返回
        if (th_.joinable()) th_.join();
    }

    int port() const { return port_; }

    // 预置"已存在的代理"（getMediaList 返回；幂等用例）
    void seed_proxy(const std::string& app, const std::string& stream) {
        std::lock_guard<std::mutex> lk(mu_);
        existing_.emplace_back(app, stream);
    }

    std::vector<std::string> deleted_keys() {
        std::lock_guard<std::mutex> lk(mu_);
        return deleted_;
    }

private:
    void accept_loop() {
        // poll 轮询（50ms）：阻塞 accept 在 Linux 上不响应他线程 close，
        // 直接 accept+close 停机会让 join 永久卡死（板上实测踩中）
        while (running_.load()) {
            pollfd pf{};
            pf.fd = listen_fd_;
            pf.events = POLLIN;
            const int pr = ::poll(&pf, 1, 50);
            if (pr <= 0) continue;
            if (listen_fd_ < 0) break;  // stop() 已关监听
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            handle_conn(fd);
            ::close(fd);
        }
    }

    static bool read_request(int fd, std::string* method, std::string* path,
                             std::vector<std::pair<std::string, std::string>>* params) {
        std::string raw;
        char buf[2048];
        size_t hdr_end = std::string::npos;
        while (hdr_end == std::string::npos) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            raw.append(buf, static_cast<size_t>(n));
            hdr_end = raw.find("\r\n\r\n");
            if (raw.size() > 65536) return false;
        }
        const size_t sp1 = raw.find(' ');
        const size_t sp2 = raw.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
        *method = raw.substr(0, sp1);
        *path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string body;
        const size_t cl = raw.find("Content-Length:");
        if (cl != std::string::npos && cl < hdr_end) {
            const int len = std::atoi(raw.c_str() + cl + 15);
            while (raw.size() < hdr_end + 4 + static_cast<size_t>(len)) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                raw.append(buf, static_cast<size_t>(n));
            }
            body = raw.substr(hdr_end + 4, static_cast<size_t>(len));
        }
        auto parse_kv = [params](const std::string& src) {
            size_t pos = 0;
            while (pos < src.size()) {
                const size_t amp = src.find('&', pos);
                const std::string pair =
                    src.substr(pos, amp == std::string::npos ? std::string::npos
                                                             : amp - pos);
                const size_t eq = pair.find('=');
                if (eq != std::string::npos)
                    params->emplace_back(url_decode(pair.substr(0, eq)),
                                         url_decode(pair.substr(eq + 1)));
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        };
        const size_t q = path->find('?');
        if (q != std::string::npos) {
            parse_kv(path->substr(q + 1));
            *path = path->substr(0, q);
        }
        parse_kv(body);
        return true;
    }

    void handle_conn(int fd) {
        // 收包超时：异常半开连接不拖死 accept 线程（下个连接还能被服务）
        timeval tv{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::string method, path;
        std::vector<std::pair<std::string, std::string>> params;
        if (!read_request(fd, &method, &path, &params)) return;
        (void)method;  // mock 不分派 method（路径即路由）
        const std::string body = route(path, params);
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n" + body;
        size_t off = 0;
        while (off < resp.size()) {
            const ssize_t n = ::send(fd, resp.data() + off, resp.size() - off, 0);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
    }

    std::string route(const std::string& path,
                      const std::vector<std::pair<std::string, std::string>>& params) {
        auto get = [&](const std::string& k) -> std::string {
            for (const auto& kv : params)
                if (kv.first == k) return kv.second;
            return "";
        };
        if (path == "/index/api/addStreamProxy") {
            const std::string stream = get("stream");
            const bool is_sub = stream.find("_sub") != std::string::npos;
            (is_sub ? add_sub : add_main)++;
            if (is_sub && fail_sub_add.load())
                return "{\"code\":-1,\"msg\":\"mock sub fail\"}";
            std::lock_guard<std::mutex> lk(mu_);
            existing_.emplace_back(get("app"), stream);
            return "{\"code\":0,\"data\":{\"key\":\"__defaultVhost__/live/" + stream +
                   "\"}}";
        }
        if (path == "/index/api/delStreamProxy") {
            ++del;
            const std::string key = get("key");
            std::lock_guard<std::mutex> lk(mu_);
            deleted_.push_back(key);
            // 从 existing_ 摘除（key = vhost/app/stream）
            const size_t p1 = key.find('/');
            const size_t p2 = key.find('/', p1 + 1);
            if (p1 != std::string::npos && p2 != std::string::npos) {
                const std::string app = key.substr(p1 + 1, p2 - p1 - 1);
                const std::string stream = key.substr(p2 + 1);
                for (size_t i = 0; i < existing_.size(); ++i)
                    if (existing_[i].first == app && existing_[i].second == stream) {
                        existing_.erase(existing_.begin() + static_cast<long>(i));
                        break;
                    }
            }
            return "{\"code\":0}";
        }
        if (path == "/index/api/getMediaList") {
            std::lock_guard<std::mutex> lk(mu_);
            std::string arr;
            for (const auto& kv : existing_) {
                if (!arr.empty()) arr += ",";
                arr +=
                    "{\"app\":\"" + kv.first + "\",\"stream\":\"" + kv.second + "\"}";
            }
            return "{\"code\":0,\"data\":[" + arr + "]}";
        }
        if (path == "/index/api/getServerConfig") return "{\"code\":0,\"data\":{}}";
        return "{\"code\":-1,\"msg\":\"mock unknown path\"}";
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread th_;
    std::mutex mu_;
    std::vector<std::pair<std::string, std::string>> existing_;
    std::vector<std::string> deleted_;
};

}  // namespace testutil
}  // namespace aibox
