#include "test_util.h"
#include "ws_server.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace aibox::biz;
namespace {
template <class Predicate> bool until(Predicate predicate, int ms = 2000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
int free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t size = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}
const std::string request = "GET /ws/events HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                            "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
struct Client {
    int fd = -1;
    std::string rx;
    explicit Client(int port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    }
    ~Client() {
        if (fd >= 0)
            ::close(fd);
    }
    bool send(const std::string& data) {
        size_t offset = 0;
        while (offset < data.size()) {
            auto n = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            offset += static_cast<size_t>(n);
        }
        return true;
    }
    bool receive(const std::function<bool()>& complete) {
        return until([&] {
            if (complete())
                return true;
            char buffer[4096];
            auto n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n > 0)
                rx.append(buffer, static_cast<size_t>(n));
            return complete();
        });
    }
    bool handshake(const std::string& path = "/ws/events") {
        auto header = request;
        header.replace(header.find("/ws/events"), 10, path);
        if (!send(header) || !receive([&] { return rx.find("\r\n\r\n") != std::string::npos; }))
            return false;
        const auto end = rx.find("\r\n\r\n") + 4;
        bool ok = rx.substr(0, end).find("101 Switching Protocols") != std::string::npos;
        rx.erase(0, end);
        return ok;
    }
    bool frame(std::string expected, uint8_t opcode = 1) {
        size_t size = 0, header = 0;
        if (!receive([&] {
                if (rx.size() < 2)
                    return false;
                size = static_cast<uint8_t>(rx[1]) & 127;
                header = 2;
                if (size == 126) {
                    if (rx.size() < 4)
                        return false;
                    size = (static_cast<uint8_t>(rx[2]) << 8) | static_cast<uint8_t>(rx[3]);
                    header = 4;
                }
                return size < 65536 && rx.size() >= header + size;
            }))
            return false;
        bool ok =
            static_cast<uint8_t>(rx[0]) == (0x80 | opcode) && rx.substr(header, size) == expected;
        rx.erase(0, header + size);
        return ok;
    }
    bool closed() {
        return until([&] {
            char data[4096];
            const auto n = ::recv(fd, data, sizeof(data), MSG_DONTWAIT);
            return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        });
    }
};
std::string masked(uint8_t opcode, bool fin, const std::string& payload) {
    std::string bytes{static_cast<char>((fin ? 128 : 0) | opcode)};
    if (payload.size() < 126)
        bytes += static_cast<char>(128 | payload.size());
    else {
        bytes += static_cast<char>(128 | 126);
        bytes += static_cast<char>(payload.size() >> 8);
        bytes += static_cast<char>(payload.size());
    }
    bytes.append("mask", 4);
    for (size_t i = 0; i < payload.size(); ++i)
        bytes += payload[i] ^ "mask"[i % 4];
    return bytes;
}

// The first data write really sends 3 bytes; the next syscall returns EAGAIN until released.
struct Transport {
    std::atomic<int> phase{0}, blocked_fd{-1};
    ssize_t send(int fd, const void* data, size_t size, int flags) {
        if (phase == 1) {
            blocked_fd = fd;
            auto n = ::send(fd, data, std::min(size, size_t{3}), flags);
            if (n > 0)
                phase = 2;
            return n;
        }
        if (phase == 2 && fd == blocked_fd) {
            errno = EAGAIN;
            return -1;
        }
        return ::send(fd, data, size, flags);
    }
};

void partial_meta(bool byte_limit) {
    Transport transport;
    WsServer::Limits limits;
    limits.max_tx_frames = byte_limit ? 20 : 3;
    limits.max_tx_bytes = byte_limit ? 256 : 4096;
    WsServer ws({}, limits, [&](int fd, const void* p, size_t n, int f) {
        return transport.send(fd, p, n, f);
    });
    const auto port = free_port();
    CHECK(ws.start("127.0.0.1", port));
    Client slow(port), healthy(port);
    CHECK(slow.handshake("/ws/meta/7"));
    CHECK(healthy.handshake("/ws/meta/8"));
    transport.phase = 1;
    const std::string active(80, 'a');
    ws.send_meta(7, active);
    CHECK(until([&] { return transport.phase == 2; }));
    for (char value : {'b', 'c', 'd', 'e', 'f'})
        ws.send_meta(7, std::string(80, value));
    CHECK(ws.stats().dropped_meta == 3);
    CHECK(ws.stats().buffered_bytes == 246);
    CHECK(ws.stats().buffered_frames == 3);
    ws.send_meta(8, "healthy");
    CHECK(healthy.frame("healthy"));
    transport.phase = 0;
    CHECK(slow.frame(active));
    CHECK(slow.frame(std::string(80, 'e')));
    CHECK(slow.frame(std::string(80, 'f')));
    CHECK(until([&] { return ws.stats().buffered_bytes == 0; }));
    CHECK(ws.stats().buffered_frames == 0);
    ws.send_meta(7, std::string(5000, 'x'));
    CHECK(ws.stats().dropped_meta == 4);
    CHECK(ws.stats().buffered_bytes == 0);
    ws.stop();
    ws.stop();
}

void slow_events() {
    Transport transport;
    WsServer::Limits limits;
    limits.max_tx_frames = 3;
    WsServer ws({}, limits, [&](int fd, const void* p, size_t n, int f) {
        return transport.send(fd, p, n, f);
    });
    int port = free_port();
    CHECK(ws.start("127.0.0.1", port));
    Client slow(port);
    CHECK(slow.handshake());
    transport.phase = 1;
    ws.broadcast(std::string(80, 'a'));
    CHECK(until([&] { return transport.phase == 2; }));
    Client healthy(port);
    CHECK(healthy.handshake());
    for (int i = 0; i < 3; ++i) {
        const auto value = std::to_string(i);
        ws.broadcast(value);
        CHECK(healthy.frame(value));
    }
    CHECK(until([&] { return ws.conn_count() == 1; }));
    CHECK(slow.closed());
    CHECK(ws.stats().slow_connections == 1);
    CHECK(ws.stats().dropped_meta == 0);
    CHECK(ws.stats().buffered_bytes == 0);
    ws.broadcast("still alive");
    CHECK(healthy.frame("still alive"));
    ws.stop();
}

void input_boundaries() {
    WsServer::Limits limits;
    limits.max_frame_bytes = 256;
    limits.max_message_bytes = 12;
    limits.max_handshake_bytes = 256;
    WsServer ws({}, limits);
    int port = free_port();
    CHECK(ws.start("127.0.0.1", port));
    Client healthy(port);
    CHECK(healthy.handshake());
    auto reject = [&](const std::string& bytes) {
        Client bad(port);
        CHECK(bad.handshake());
        CHECK(bad.send(bytes));
        CHECK(bad.closed());
    };
    reject(masked(1, false, "12345678") + masked(0, true, "12345"));
    reject(masked(0, true, "orphan"));
    reject(masked(1, false, "a") + masked(1, true, "new"));
    reject(masked(9, false, "control"));
    reject(masked(9, true, std::string(126, 'x')));
    reject(std::string("\x89\x00", 2)); // all client frames require masking, including control
    reject(std::string("\x81\xff\x00\x00\x00\x00\x00\x00\x01\x01", 10));
    reject(std::string("\x81\xfe\x01\x01", 4)); // Reject declared 257-byte frame before body arrives.
    auto rsv = masked(1, true, "x");
    rsv[0] |= 0x40;
    reject(rsv);
    {
        Client bad(port);
        CHECK(bad.send(std::string(257, 'x')));
        CHECK(bad.closed());
    }
    {
        Client bad(port);
        auto header = request;
        header.insert(header.size() - 2, "X-Pad: " + std::string(256, 'x') + "\r\n");
        CHECK(bad.send(header));
        CHECK(bad.closed());
    }
    for (const std::string path : {"/ws/meta/7abc", "/ws/meta/-1", "/ws/meta/4294967296"}) {
        Client bad(port);
        auto header = request;
        header.replace(header.find("/ws/events"), 10, path);
        CHECK(bad.send(header));
        CHECK(bad.closed());
    }
    {
        Client valid(port);
        CHECK(valid.handshake());
        CHECK(valid.send(masked(1, false, "123456") + masked(9, true, "ok") +
                         masked(0, true, "789012")));
        CHECK(
            valid.frame("ok", 10)); // control can interleave without resetting fragment accounting
    }
    ws.broadcast("healthy");
    CHECK(healthy.frame("healthy"));
    CHECK(ws.stats().invalid_input >= 13);
    const auto invalid = ws.stats().invalid_input;
    CHECK(healthy.send(masked(8, true, "")));
    CHECK(healthy.frame("", 8));
    CHECK(healthy.closed());
    CHECK(ws.stats().invalid_input == invalid);
    ws.stop();
}

void connection_cap_and_hooks() {
    WsServer::Limits limits;
    limits.max_connections = 1;
    limits.handshake_timeout_ms = 150;
    WsServer ws({}, limits);
    int port = free_port();
    CHECK(ws.start("127.0.0.1", port));
    {
        Client incomplete(port);
        CHECK(until([&] { return ws.conn_count() == 1; }));
        Client excess(port);
        CHECK(excess.closed());
        CHECK(ws.stats().rejected_connections == 1);
        CHECK(incomplete.closed());
        CHECK(until([&] { return ws.conn_count() == 0; }));
    }
    ws.stop();
    std::atomic<int> sub{0}, unsub{0};
    WsServer reentrant({});
    reentrant.set_hooks({[&](uint32_t cam) {
                             (void)reentrant.conn_count();
                             reentrant.send_meta(cam, "subscribed");
                             ++sub;
                         },
                         [&](uint32_t) {
                             (void)reentrant.stats();
                             ++unsub;
                         }});
    CHECK(reentrant.start("127.0.0.1", port));
    Client viewer(port);
    CHECK(viewer.handshake("/ws/meta/5"));
    CHECK(viewer.frame("subscribed"));
    reentrant.stop();
    reentrant.stop();
    CHECK(sub == 1);
    CHECK(unsub == 1);
    CHECK(reentrant.stats().buffered_bytes == 0);
}

void partial_handshake() {
    Transport transport;
    transport.phase = 1;
    WsServer ws({}, WsServer::Limits{}, [&](int fd, const void* p, size_t n, int f) {
        return transport.send(fd, p, n, f);
    });
    int port = free_port();
    CHECK(ws.start("127.0.0.1", port));
    Client client(port);
    CHECK(client.send(request));
    CHECK(until([&] { return transport.phase == 2; }));
    ws.broadcast("after handshake");
    transport.phase = 0;
    CHECK(client.receive([&] { return client.rx.find("\r\n\r\n") != std::string::npos; }));
    CHECK(client.rx.find("HTTP/1.1 101") == 0);
    client.rx.erase(0, client.rx.find("\r\n\r\n") + 4);
    CHECK(client.frame("after handshake"));
    ws.stop();
}
} // namespace
int main() {
    partial_meta(false);
    partial_meta(true);
    slow_events();
    input_boundaries();
    connection_cap_and_hooks();
    partial_handshake();
    return g_fail;
}
