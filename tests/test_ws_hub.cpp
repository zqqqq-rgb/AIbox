// T4.5 单测（§4-T4.5 断言表）：
//   ① WS 握手（RFC 6455 标准算例 Accept 值）+ broadcast 收帧 + ping→pong
//   ② 引用计数：双 viewer 同通道 sub_meta 恰 1 次、全断 unsub 1 次、组播双达
//   ③ 非法路径握手拒绝
//   ④ HookServer：on_stream_changed（form/json 双格式）、on_record_mp4 → segments 插入
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <json/json.h>

#include "hook_server.h"
#include "test_util.h"
#include "ws_server.h"

using namespace aibox;
using namespace aibox::biz;

namespace {

int free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

template <typename Pred>
bool wait_for(Pred p, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

// ---- 极简 WS 客户端（测试专用：掩码上行 + 帧解析下行）----
class WsClient {
public:
    bool handshake(int port, const std::string& path, std::string* accept_out) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        // 握手全程 3s 超时：服务器不应答时 fail-fast（卡死=测试挂到 timeout）
        timeval tv{3, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
        const std::string req =
            "GET " + path +
            " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (::send(fd_, req.data(), req.size(), 0) < 0) return false;
        // 读响应头（\r\n\r\n 定界）
        std::string resp;
        char buf[1024];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            resp.append(buf, static_cast<size_t>(n));
            if (resp.size() > 8192) return false;
        }
        if (resp.find("101") == std::string::npos) return false;
        if (accept_out) {
            const size_t k = resp.find("Sec-WebSocket-Accept: ");
            if (k != std::string::npos) {
                const size_t v = k + strlen("Sec-WebSocket-Accept: ");
                const size_t e = resp.find("\r\n", v);
                *accept_out = resp.substr(v, e - v);
            }
        }
        return true;
    }

    // 服务端帧不掩码；timeout 后无完整帧返回 false
    bool read_frame(uint8_t* opcode, std::string* payload, int timeout_ms) {
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        auto recv_some = [&](std::string& buf) -> bool {
            char b[4096];
            const ssize_t n = ::recv(fd_, b, sizeof(b), 0);
            if (n <= 0) return false;
            buf.append(b, static_cast<size_t>(n));
            return true;
        };
        std::string raw = std::move(pending_);
        pending_.clear();
        for (;;) {
            if (raw.size() >= 2) {
                const uint8_t op = static_cast<uint8_t>(raw[0]) & 0x0F;
                const uint8_t len0 = static_cast<uint8_t>(raw[1]) & 0x7F;
                size_t off = 2;
                uint64_t len = len0;
                if (len0 == 126) {
                    if (raw.size() < 4) {
                        if (!recv_some(raw)) return false;
                        continue;
                    }
                    len = (static_cast<uint8_t>(raw[2]) << 8) |
                          static_cast<uint8_t>(raw[3]);
                    off = 4;
                }
                if (raw.size() >= off + len) {
                    *opcode = op;
                    *payload = raw.substr(off, static_cast<size_t>(len));
                    pending_ = raw.substr(off + static_cast<size_t>(len));
                    return true;
                }
            }
            if (!recv_some(raw)) return false;
        }
    }

    // 上行必须掩码（RFC 6455 §5.1）
    bool send_frame(uint8_t opcode, const std::string& payload) {
        std::string out;
        out.push_back(static_cast<char>(0x80 | opcode));
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        if (payload.size() < 126) {
            out.push_back(static_cast<char>(0x80 | payload.size()));
        } else {
            out.push_back(static_cast<char>(0x80 | 126));
            out.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
            out.push_back(static_cast<char>(payload.size() & 0xFF));
        }
        out.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < payload.size(); ++i)
            out.push_back(payload[i] ^ mask[i % 4]);
        return ::send(fd_, out.data(), out.size(), 0) >= 0;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
    std::string pending_;
};

// ① 握手 + broadcast + ping/pong
void test_handshake_and_frames() {
    WsServer::Hooks hooks;
    WsServer ws(hooks);
    const int port = free_port();
    CHECK(ws.start("127.0.0.1", port));

    WsClient c1;
    std::string accept;
    CHECK(c1.handshake(port, "/ws/events", &accept));
    // RFC 6455 §1.3 标准算例：key=dGhlIHNhbXBsZSBub25jZQ== 的期望 Accept
    CHECK(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    ws.broadcast("{\"evt\":\"camera_status\"}");
    uint8_t op = 0;
    std::string payload;
    CHECK(c1.read_frame(&op, &payload, 3000));
    CHECK(op == 0x1);
    CHECK(payload == "{\"evt\":\"camera_status\"}");

    // ping → pong（同 payload 回显）
    CHECK(c1.send_frame(0x9, "hb"));
    CHECK(c1.read_frame(&op, &payload, 3000));
    CHECK(op == 0xA);
    CHECK(payload == "hb");
    c1.close();
    ws.stop();
}

// ② 引用计数（§4-T4.5：双 viewer 只 sub 1 次，全断 unsub）
void test_reference_counting() {
    std::atomic<int> sub_cnt{0}, unsub_cnt{0};
    WsServer::Hooks hooks;
    hooks.on_sub = [&](uint32_t) { ++sub_cnt; };
    hooks.on_unsub = [&](uint32_t) { ++unsub_cnt; };
    WsServer ws(hooks);
    const int port = free_port();
    CHECK(ws.start("127.0.0.1", port));

    WsClient a, b;
    CHECK(a.handshake(port, "/ws/meta/7", nullptr));
    CHECK(wait_for([&] { return sub_cnt.load() == 1; }, 2000));  // 首个 viewer → sub×1
    CHECK(b.handshake(port, "/ws/meta/7", nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(sub_cnt.load() == 1);   // 第二个 viewer 不再 sub（引用计数）

    // 组播双达
    ws.send_meta(7, "{\"targets\":[]}");
    uint8_t op = 0;
    std::string payload;
    CHECK(a.read_frame(&op, &payload, 3000) && payload == "{\"targets\":[]}");
    CHECK(b.read_frame(&op, &payload, 3000) && payload == "{\"targets\":[]}");

    // 断开一个：仍订阅（不 unsub）
    a.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(unsub_cnt.load() == 0);

    // 全断 → unsub×1
    b.close();
    CHECK(wait_for([&] { return unsub_cnt.load() == 1; }, 2000));
    CHECK(ws.conn_count() == 0);
    ws.stop();
}

// ③ 非法路径拒绝
void test_bad_path_rejected() {
    WsServer::Hooks hooks;
    WsServer ws(hooks);
    const int port = free_port();
    CHECK(ws.start("127.0.0.1", port));
    WsClient c;
    CHECK(!c.handshake(port, "/ws/other", nullptr));  // 非 101 → false
    c.close();
    ws.stop();
}

// ④ HookServer（form + json 双格式；on_record_mp4 → segments）
void test_hook_server() {
    Db db(":memory:");
    CHECK(db.migrate() == Db::kSchemaLatest);

    HookServer::Hooks hooks;
    std::vector<std::pair<int64_t, bool>> status_events;
    hooks.on_stream_changed = [&](int64_t cam, bool online) {
        status_events.emplace_back(cam, online);
    };
    hooks.on_record_mp4 = [&](const SegmentRow& seg) { db.insert_segment(seg); };

    HookServer::Params p;
    p.port = free_port();
    HookServer hook(hooks, p);
    CHECK(hook.start());

    // client 作用域必须先于 hook.stop()：老 httplib 的 stop() join keep-alive
    // worker 时等客户端先关连接——client 活得比 stop 长会死锁（板上实测卡死）
    {
        httplib::Client cli("127.0.0.1", p.port);
        // form 格式（ZLM 传统）
        auto res = cli.Post("/hook/on_stream_changed",
                            "stream=cam1_sub&regist=1",
                            "application/x-www-form-urlencoded");
        CHECK(res && res->status == 200);
        CHECK(res->body.find("\"code\":0") != std::string::npos);
        CHECK(status_events.size() == 1 && status_events[0].first == 1 &&
              status_events[0].second);

        // json 格式
        res = cli.Post("/hook/on_stream_changed",
                       R"({"stream":"cam2_sub","regist":false})", "application/json");
        CHECK(status_events.size() == 2 && !status_events[1].second);

        // on_record_mp4 → segments 插入（start_time 秒 → ms）
        res = cli.Post("/hook/on_record_mp4",
                       "stream=cam3_main&file=/data/mp4/x.mp4&start_time=1700000000&time_len=300.5",
                       "application/x-www-form-urlencoded");
        CHECK(res && res->body.find("\"code\":0") != std::string::npos);
        const auto segs = db.list_segments(3, 0, 0, 10);
        CHECK(segs.size() == 1);
        CHECK(segs[0].start_ts == 1700000000000LL);
        CHECK(segs[0].duration_s > 300.4 && segs[0].duration_s < 300.6);

        // 未实现 hook → code 0（防重试风暴）
        res = cli.Post("/hook/on_publish", "x=1", "application/x-www-form-urlencoded");
        CHECK(res && res->body.find("\"code\":0") != std::string::npos);
    }  // cli 先析构，释放 keep-alive 连接
    hook.stop();
}

void run() {
    fprintf(stderr, "[diag] test_ws_hub v4 (handshake-3s-timeout + server-rx-log)\n");
    struct Mark {
        const char* name;
        explicit Mark(const char* n) : name(n) {
            fprintf(stderr, "[diag] >> %s enter\n", name);
        }
        ~Mark() { fprintf(stderr, "[diag] << %s exit\n", name); }
    };
    {
        Mark m("handshake_and_frames");
        test_handshake_and_frames();
    }
    {
        Mark m("reference_counting");
        test_reference_counting();
    }
    {
        Mark m("bad_path_rejected");
        test_bad_path_rejected();
    }
    {
        Mark m("hook_server");
        test_hook_server();
    }
}

}  // namespace

TEST_MAIN("ws_hub")
