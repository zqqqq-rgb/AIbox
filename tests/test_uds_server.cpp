// T2.5 单测（任务分解 §4-T2.5）：UdsServer loopback 自连（POSIX socket 客户端）
// 覆盖：①订阅过滤（sub_meta 后只收所订 cam）②慢消费者隔离（队列满丢最旧计数，
//       不阻塞 emit、不拖累其他连接）③坏 JSON 回 4001 后服务端断开（详设 §3.10）
//       ④cmd 往返（含无 handler → ret=1001）⑤unsub_meta 后收不到
// 约定：服务端空闲心跳禁用（构造传 0，B 全程不读也不能被心跳踢掉）；
//       每个 recv 2s 超时（SO_RCVTIMEO）防挂死
#include "ipc/UdsServer.h"
#include "ipc/Protocol.h"
#include "test_util.h"

#include <json/json.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

using namespace aibox;

namespace {

const char* kSockPath = "/tmp/aibox_test_uds.sock";

// 块读余量缓存：recv 一块可能越过行边界，剩余字节留到下一次 read_line
// （单线程测试，按 fd 存；连接前清除防 fd 复用串扰）
std::unordered_map<int, std::string> g_pending;

int connect_client() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return -1;
    g_pending.erase(fd);
    sockaddr_un addr{};  // 零初始化，sun_path 天然以 '\0' 结尾
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, kSockPath, std::strlen(kSockPath));
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        CHECK(false);  // connect 失败
        ::close(fd);
        return -1;
    }
    timeval tv{};  // 每个 recv 2s 超时防挂死
    tv.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

bool write_line(int fd, const std::string& s) {
    std::string wire = s;
    wire.push_back('\n');
    size_t off = 0;
    while (off < wire.size()) {  // 客户端为阻塞 socket，一次 write 即可；循环兜底
        const ssize_t n = ::write(fd, wire.data() + off, wire.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// 读一行（到 '\n' 为止）；超时（EAGAIN）或 EOF 返回 false
bool read_line(int fd, std::string* out) {
    out->clear();
    std::string& p = g_pending[fd];
    for (;;) {
        const size_t nl = p.find('\n');
        if (nl != std::string::npos) {
            out->assign(p, 0, nl);
            p.erase(0, nl + 1);
            return true;
        }
        char buf[8192];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;  // 超时 / EOF / 错误（部分行留 pending）
        p.append(buf, static_cast<size_t>(n));
    }
}

// 连接是否仍存活：有数据或 EAGAIN = 活；recv 返回 0 = 服务端已关
bool still_open(int fd) {
    char ch = 0;
    const ssize_t n = ::recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return true;
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

Json::Value parse_line(const std::string& line) {
    Json::Value root;
    Json::CharReaderBuilder b;
    Json::String errs;
    const std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    const bool ok = rd->parse(line.data(), line.data() + line.size(), &root, &errs);
    CHECK(ok);
    return root;
}

// 手拼 detections 事件（emit 收预序列化 JSON 片段，server 原样回发）；pad 撑大帧
// 便于撑爆「内核发送缓冲 + 1024 条队列」，稳定触发丢最旧
std::string make_det(uint32_t cam, int seq, size_t pad) {
    std::string s = "{\"evt\":\"detections\",\"id\":" + std::to_string(cam) +
                    ",\"ts\":" + std::to_string(seq) + ",\"targets\":[],\"pad\":\"";
    s.append(pad, 'x');
    s += "\"}";
    return s;
}

void run() {
    ::unlink(kSockPath);  // start 内部也会清，双保险

    UdsServer server(0);  // 空闲心跳禁用（B 不读不发，不能被心跳踢掉）

    // 注册表（详设 §3.11 main 装配同款）：sub/unsub 回 ack；get_metrics 带内联 data
    server.on_cmd("sub_meta", [](const proto::ParsedMsg&) { return proto::Ack{}; });
    server.on_cmd("unsub_meta", [](const proto::ParsedMsg&) { return proto::Ack{}; });
    server.on_cmd("get_metrics", [](const proto::ParsedMsg&) {
        proto::Ack a;
        a.ret = proto::kOk;
        a.has_data = true;
        a.data = "{\"ok\":true}";
        return a;
    });

    CHECK(server.start(kSockPath));
    CHECK(server.start(kSockPath));  // start 幂等

    std::string l;

    // ---- ① 订阅过滤：A 订 cam1 → 只收 cam1 的 detections ----
    int a = connect_client();
    CHECK(a >= 0);
    {
        proto::SubMeta sm;
        sm.id = 1;
        sm.subscribe = true;
        CHECK(write_line(a, proto::encode_request(1, sm)));
    }
    CHECK(read_line(a, &l));  // 读 sub ack 兼作「订阅已生效」同步点
    {
        const Json::Value r = parse_line(l);
        CHECK(r["ack"].asUInt64() == 1 && r["ret"].asInt() == proto::kOk);
    }
    for (int i = 0; i < 10; ++i) server.emit(make_det(1, i, 0), 1);  // 已订阅
    for (int i = 0; i < 10; ++i) server.emit(make_det(2, i, 0), 2);  // 未订阅 → 应被过滤
    {
        int cam1 = 0, other = 0;
        for (int i = 0; i < 10; ++i) {
            CHECK(read_line(a, &l));
            const Json::Value e = parse_line(l);
            if (e["evt"].asString() == "detections" && e["id"].asUInt() == 1) {
                ++cam1;
            } else {
                ++other;
            }
        }
        CHECK(cam1 == 10 && other == 0);  // 恰好 10 条且都是 cam1
        CHECK(!read_line(a, &l));         // cam2 全被过滤、无多余帧（此处等满 2s 超时）
        CHECK(still_open(a));             // 是超时不是断连
    }

    // ---- ② 慢消费者隔离：B 订阅后不 read，emit 2000 条不阻塞、A 照常收到 ----
    int b = connect_client();
    CHECK(b >= 0);
    {
        proto::SubMeta sm;
        sm.id = 1;
        sm.subscribe = true;
        CHECK(write_line(b, proto::encode_request(2, sm)));
    }
    CHECK(read_line(b, &l));  // 仅作订阅生效同步，此后 B 永不再读（慢消费者）
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 2000; ++i) server.emit(make_det(1, i, 8192), 1);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms < 2000);  // emit 不阻塞：B 的队列满只丢最旧，不拖累生产者
    }
    {
        for (int i = 0; i < 10; ++i) {  // A 再收 10 条：全是 cam1（B 慢不拖累 A）
            CHECK(read_line(a, &l));
            const Json::Value e = parse_line(l);
            CHECK(e["evt"].asString() == "detections" && e["id"].asUInt() == 1);
        }
        CHECK(server.total_dropped() > 0);  // 慢消费者（A/B 队列 1024 上限）丢帧累计
    }

    // ---- ③ 坏 JSON：回 ret=4001 后服务端关闭该连接 ----
    {
        int c = connect_client();
        CHECK(c >= 0);
        CHECK(write_line(c, "garbage"));
        CHECK(read_line(c, &l));
        const Json::Value r = parse_line(l);
        CHECK(r["ret"].asInt() == proto::kErrProtocol);
        CHECK(r["ack"].asUInt64() == 0);       // 坏 JSON 无 msg_id → ack:0
        CHECK(r["msg"].asString().size() > 0);
        char ch = 0;
        CHECK(::recv(c, &ch, 1, 0) == 0);  // 4001 回完后服务端断开 → read 返回 0（EOF）
        ::close(c);
    }

    // ---- ④ cmd 往返：handler 回 Ack{ret:0, data}；无注册 cmd 回 1001 ----
    int d = connect_client();
    CHECK(d >= 0);
    CHECK(write_line(d, proto::encode_request(77, proto::GetMetrics{})));
    CHECK(read_line(d, &l));
    {
        const Json::Value r = parse_line(l);
        CHECK(r["ack"].asUInt64() == 77);
        CHECK(r["ret"].asInt() == proto::kOk);
        CHECK(r["data"]["ok"].asBool());  // data 为 §8.1 内联 JSON 对象
    }
    {
        proto::AddCam ac;  // 合法 cmd 但未注册 handler → ret=1001（不断开）
        ac.id = 9;
        ac.rtsp_sub = "rtsp://s";
        CHECK(write_line(d, proto::encode_request(78, ac)));
        CHECK(read_line(d, &l));
        const Json::Value r = parse_line(l);
        CHECK(r["ack"].asUInt64() == 78 && r["ret"].asInt() == 1001);
        CHECK(still_open(d));
    }

    // ---- ⑤ unsub：A 退订 cam1 后 emit → 收不到 ----
    while (read_line(a, &l)) {
    }  // 排干 ② 留给 A 的积压（读到 2s 超时为止）
    CHECK(still_open(a));
    {
        proto::SubMeta um;
        um.id = 1;
        um.subscribe = false;
        CHECK(write_line(a, proto::encode_request(9, um)));
    }
    CHECK(read_line(a, &l));  // 读 unsub ack 兼作「退订已生效」同步点
    {
        const Json::Value r = parse_line(l);
        CHECK(r["ack"].asUInt64() == 9 && r["ret"].asInt() == proto::kOk);
    }
    for (int i = 0; i < 3; ++i) server.emit(make_det(1, 100 + i, 0), 1);
    CHECK(!read_line(a, &l));  // 退订后收不到（等满 2s 超时）
    CHECK(still_open(a));      // unsub 不断连

    // ---- 收尾：连接台账 + stop 幂等 ----
    CHECK(server.conn_count() == 3);  // A、B、D（C 已因协议错误被服务端关闭）
    server.stop();
    CHECK(server.conn_count() == 0);
    server.stop();  // stop 幂等
    ::close(a);
    ::close(b);
    ::close(d);
}

}  // namespace

TEST_MAIN("test_uds_server")
