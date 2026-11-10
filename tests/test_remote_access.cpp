#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <httplib.h>
#include <json/json.h>

#include "access_session.h"
#include "fake_engine.h"
#include "http_api.h"
#include "mock_zlm.h"
#include "test_util.h"
#include "uds_client.h"
#include "ws_server.h"
#include "zlm_client.h"

using namespace aibox;
using namespace aibox::biz;
namespace {
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (0)
int port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address); REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    close(fd); return ntohs(address.sin_port);
}
template<class F> bool until(F predicate) {
    for (int i = 0; i < 400; ++i) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return predicate();
}
Json::Value json(const std::string& body) {
    Json::CharReaderBuilder builder; auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    Json::Value value; std::string error; REQUIRE(reader->parse(body.data(), body.data() + body.size(), &value, &error)); return value;
}
int ws_connect(int listener, const std::string& cookie, const std::string& origin, std::string& response) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval timeout{1, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(listener);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    std::string request = "GET /ws/meta/1 HTTP/1.1\r\nHost: aibox.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    if (!cookie.empty()) request += "Cookie: " + cookie + "\r\n";
    if (!origin.empty()) request += "Origin: " + origin + "\r\n";
    request += "\r\n";
    REQUIRE(send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
    response.clear(); char buffer[1024];
    while (response.find("\r\n\r\n") == std::string::npos && response.size() < 8192) {
        const auto count = recv(fd, buffer, sizeof(buffer), 0); if (count <= 0) break; response.append(buffer, count);
    }
    return fd;
}
void run() {
    Db db(":memory:"); REQUIRE(db.migrate() == Db::kSchemaLatest);
    testutil::MockZlm media; REQUIRE(media.start());
    testutil::FakeEngine engine;
    const auto socket_path = "/tmp/aibox_remote_" + std::to_string(getpid()) + ".sock";
    REQUIRE(engine.start(socket_path));
    UdsClient uds(socket_path, db, {}, UdsClient::Params{10, 200, 0, 10000, 3});
    uds.start(); REQUIRE(until([&] { return uds.online(); }));
    ZlmClient zlm({"127.0.0.1", media.port(), "private-zlm-secret", 300});
    const int http_port = port(), ws_port = port();
    std::atomic<int64_t> clock{0};
    AccessSession::Params params; params.token = "configured-test-secret"; params.public_origin = "https://aibox.example.com";
    params.http_port = http_port; params.ttl_ms = 1000; params.clock = [&] { return clock.load(); };
    auto access = std::make_shared<AccessSession>(params);
    std::atomic<int> subscribed{0}, unsubscribed{0};
    WsServer ws({[&](uint32_t) { ++subscribed; }, [&](uint32_t) { ++unsubscribed; }});
    ws.set_access(access); REQUIRE(ws.start("127.0.0.1", ws_port));
    char temporary[] = "/tmp/aibox_remote_files_XXXXXX"; REQUIRE(mkdtemp(temporary));
    const std::string directory = temporary, website = directory + "/web", recordings = directory + "/data";
    REQUIRE(mkdir(website.c_str(), 0700) == 0 && mkdir(recordings.c_str(), 0700) == 0);
    { std::ofstream(website + "/index.html") << "public shell"; std::ofstream(recordings + "/proof.mp4") << "private proof"; }
    HttpApi::Deps deps; deps.db = &db; deps.uds = &uds; deps.zlm = &zlm;
    deps.http_port = http_port; deps.ws_port = ws_port; deps.bind_host = "127.0.0.1";
    deps.static_dir = website; deps.recordings_dir = recordings;
    deps.auth_token = params.token; deps.public_base_url = params.public_origin; deps.access = access; deps.ws = &ws;
    HttpApi api(deps); REQUIRE(api.start());
    httplib::Client client("127.0.0.1", http_port);
    auto result = client.Get("/api/auth/status"); REQUIRE(result);
    CHECK(json(result->body)["data"]["auth_required"].asBool());
    CHECK(!json(result->body)["data"]["authenticated"].asBool());
    result = client.Get("/"); REQUIRE(result); CHECK(result->status == 200);
    for (const char* path : {"/api/system/info", "/api/auth/check", "/api/events/1", "/recordings/proof.mp4", "/api/cameras/1/snapshot"}) {
        result = client.Get(path); REQUIRE(result); CHECK(result->status == 401);
    }
    result = client.Post("/api/auth/login", "{\"token\":\"wrong\"}", "application/json"); REQUIRE(result); CHECK(result->status == 401);
    result = client.Post("/api/auth/login", {{"Origin", "https://evil.example.com"}}, "{\"token\":\"configured-test-secret\"}", "application/json");
    REQUIRE(result); CHECK(result->status == 403);
    result = client.Post("/api/auth/login", "{\"token\":\"configured-test-secret\"}", "application/json"); REQUIRE(result); REQUIRE(result->status == 200);
    const auto set_cookie = result->get_header_value("Set-Cookie");
    CHECK(set_cookie.find("HttpOnly") != std::string::npos && set_cookie.find("Secure") != std::string::npos);
    const std::string cookie = set_cookie.substr(0, set_cookie.find(';'));
    const httplib::Headers read_headers{{"Cookie", cookie}}, write_headers{{"Cookie", cookie}, {"Origin", params.public_origin}};
    result = client.Get("/api/system/info", read_headers); REQUIRE(result); CHECK(result->status == 200);
    CHECK(json(result->body)["data"]["same_origin_ws"].asBool());
    result = client.Get("/recordings/proof.mp4", read_headers); REQUIRE(result); CHECK(result->status == 200 && result->body == "private proof");
    result = client.Post("/api/auth/logout", read_headers, "{}", "application/json"); REQUIRE(result); CHECK(result->status == 403);
    result = client.Post("/api/cameras/scan", write_headers, "{\"timeout_ms\":0}", "application/json"); REQUIRE(result); CHECK(json(result->body)["code"].asInt() == 1001);
    result = client.Post("/api/cameras/scan", write_headers, "{\"unknown\":1}", "application/json"); REQUIRE(result); CHECK(json(result->body)["code"].asInt() == 1001);
    result = client.Get("/api/cameras/scan/interfaces", read_headers); REQUIRE(result); CHECK(json(result->body)["data"]["interfaces"].isArray());
    result = client.Get("/api/cameras/1/stream", read_headers); REQUIRE(result);
    const auto signaling = json(result->body)["data"]["signaling"].asString();
    CHECK(signaling == "https://aibox.example.com/media/index/api/webrtc?app=live&stream=cam1_sub&type=play");
    CHECK(signaling.find("private-zlm-secret") == std::string::npos);
    result = client.Get("/api/system/info", {{"Authorization", "Bearer " + params.token}}); REQUIRE(result); CHECK(result->status == 200);

    std::string response; int fd = ws_connect(ws_port, "", params.public_origin, response); CHECK(response.find("101") == std::string::npos); close(fd);
    fd = ws_connect(ws_port, cookie, "https://evil.example.com", response); CHECK(response.find("101") == std::string::npos); close(fd);
    CHECK(subscribed.load() == 0);
    fd = ws_connect(ws_port, cookie, params.public_origin, response); CHECK(response.find("101 Switching") != std::string::npos);
    REQUIRE(until([&] { return subscribed.load() == 1; }));
    result = client.Post("/api/auth/logout", write_headers, "{}", "application/json"); REQUIRE(result); CHECK(result->status == 200);
    CHECK(until([&] { return ws.conn_count() == 0 && unsubscribed.load() == 1; })); close(fd);
    result = client.Get("/recordings/proof.mp4", read_headers); REQUIRE(result); CHECK(result->status == 401);

    result = client.Post("/api/auth/login", "{\"token\":\"configured-test-secret\"}", "application/json"); REQUIRE(result);
    const auto second_cookie = result->get_header_value("Set-Cookie");
    fd = ws_connect(ws_port, second_cookie.substr(0, second_cookie.find(';')), params.public_origin, response); CHECK(response.find("101 Switching") != std::string::npos);
    clock = 1001; CHECK(until([&] { return ws.conn_count() == 0; })); close(fd);
    api.stop(); ws.stop(); uds.stop(); engine.stop(); media.stop();
    unlink((website + "/index.html").c_str()); unlink((recordings + "/proof.mp4").c_str());
    rmdir(website.c_str()); rmdir(recordings.c_str()); rmdir(directory.c_str());
}
}
int main() {
    try { run(); } catch (const std::exception& error) { fprintf(stderr, "remote_access: %s\n", error.what()); return 1; }
    if (!g_fail) printf("remote_access PASSED\n");
    return g_fail;
}
