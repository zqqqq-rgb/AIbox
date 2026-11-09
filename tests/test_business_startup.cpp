#include "test_util.h"
#include "db.h"
#include "fake_engine.h"
#include "mock_zlm.h"
#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <sstream>
#include <httplib.h>
#include <json/json.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
int reserve_port(int* port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t size = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    *port = ntohs(address.sin_port);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
int free_port() {
    int port = 0;
    const auto fd = reserve_port(&port);
    if (fd >= 0)
        ::close(fd);
    return port;
}
struct Child {
    std::string directory, config, log;
    pid_t pid = -1;
    Child(const char* executable, int http_port, int ws_port, int hook_port = 0,
          int zlm_port = 1, bool recordings = false) {
        char temporary[] = "/tmp/aibox_startup_XXXXXX";
        auto path = ::mkdtemp(temporary);
        if (!path) {
            CHECK(false);
            return;
        }
        directory = path;
        config = directory + "/config.json";
        log = directory + "/child.log";
        Json::Value value;
        value["db_path"] = directory + "/business.db";
        value["uds_path"] = directory + "/absent-engine.sock";
        value["http_port"] = http_port;
        value["ws_port"] = ws_port;
        value["hook_port"] = hook_port ? hook_port : free_port();
        value["bind_host"] = "127.0.0.1";
        value["zlm"]["port"] = zlm_port;
        if (recordings) value["recordings_dir"] = directory;
        std::ofstream output(config);
        output << value;
        output.close();
        const int sink = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        pid = ::fork();
        if (pid == 0) {
            ::dup2(sink, STDOUT_FILENO);
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
            ::execl(executable, executable, "--config", config.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (sink >= 0)
            ::close(sink);
    }
    bool exited(int expected, int timeout_ms = 5000) {
        int status = 0;
        for (int i = 0; i < timeout_ms / 10; ++i) {
            if (pid > 0 && ::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                const bool valid = WIFEXITED(status) && WEXITSTATUS(status) == expected;
                if (!valid)
                    fprintf(stderr, "unexpected child exit status %d\n", status);
                return valid;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    ~Child() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        for (const auto* file :
             {"config.json", "child.log", "business.db", "business.db-wal", "business.db-shm"})
            if (!directory.empty())
                ::unlink((directory + "/" + file).c_str());
        if (!directory.empty())
            ::rmdir((directory + "/snap").c_str());
        if (!directory.empty())
            ::rmdir(directory.c_str());
    }
};
bool wait_ready(httplib::Client& client, bool engine = false) {
    for (int i = 0; i < 200; ++i) {
        auto response = client.Get("/api/system/info");
        if (response && response->body.find("\"ready\":true") != std::string::npos &&
            (!engine || response->body.find("\"engine_state\":\"ONLINE\"") != std::string::npos))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
void terminate_while_waiting_for_ack(const char* executable) {
    aibox::testutil::MockZlm media;
    CHECK(media.start());
    const auto port = free_port();
    Child child(executable, port, free_port(), 0, media.port());
    aibox::testutil::FakeEngine engine;
    CHECK(engine.start(child.directory + "/absent-engine.sock"));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(0, 100000);
    CHECK(wait_ready(client, true));
    std::promise<void> reached, release;
    auto reached_future = reached.get_future();
    auto released = release.get_future().share();
    std::atomic<bool> once{false};
    engine.after_set_algos([&] {
        if (!once.exchange(true)) { reached.set_value(); released.wait(); }
    });
    auto request = std::async(std::launch::async, [&] {
        httplib::Client writer("127.0.0.1", port);
        writer.set_read_timeout(8, 0);
        auto response = writer.Post("/api/cameras",
            R"({"name":"ack shutdown","rtsp_main":"rtsp://camera/main","rtsp_sub":"rtsp://camera/sub"})",
            "application/json");
        return response ? response->body : "";
    });
    CHECK(reached_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    if (child.pid > 0) ::kill(child.pid, SIGTERM);
    CHECK(child.exited(0)); // Must not wait for the engine callback to produce its ACK.
    auto response = request.get();
    CHECK(response.find("\"accepted\":true") != std::string::npos);
    CHECK(response.find("\"code\":0") == std::string::npos);
    release.set_value();
    engine.stop(); media.stop();
}
void terminate_during_snapshot(const char* executable) {
    std::promise<void> reached, release;
    auto waiting = reached.get_future();
    auto released = release.get_future().share();
    aibox::testutil::MockZlm media;
    media.on_snapshot = [&] { reached.set_value(); released.wait(); };
    CHECK(media.start());
    const auto port = free_port();
    Child child(executable, port, free_port(), 0, media.port(), true);
    aibox::testutil::FakeEngine engine;
    CHECK(engine.start(child.directory + "/absent-engine.sock"));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(0, 100000);
    CHECK(wait_ready(client, true));
    auto response = client.Post("/api/cameras",
        R"({"name":"snapshot shutdown","rtsp_main":"rtsp://camera/main","rtsp_sub":"rtsp://camera/sub"})",
        "application/json");
    Json::Value root;
    if (response) { std::istringstream input(response->body); input >> root; }
    CHECK(root["code"].asInt() == 0);
    const auto id = root["data"]["id"].asInt64();
    CHECK(id > 0);
    aibox::proto::RuleEventEvt event;
    event.id = static_cast<uint32_t>(id);
    event.algo = "person"; event.type = "region_in";
    event.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    engine.emit(aibox::proto::encode_event(event));
    CHECK(waiting.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    // First task is executing; 128 wait and 2 overflow. All events must remain indexed.
    for (int i = 0; i < 130; ++i) { ++event.ts; engine.emit(aibox::proto::encode_event(event)); }
    aibox::biz::Db retained(child.directory + "/business.db");
    bool all_retained = false;
    for (int i = 0; i < 200; ++i) {
        if (retained.list_events(id, "", 0, 0, 200).size() == 131) {
            all_retained = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(all_retained);
    if (child.pid > 0) ::kill(child.pid, SIGTERM);
    CHECK(child.exited(0, 8000)); // getSnap's 3s wait plus 2s transport allowance is finite.
    CHECK(retained.list_events(id, "", 0, 0, 200).size() == 131);
    std::ifstream log(child.log);
    const std::string contents((std::istreambuf_iterator<char>(log)), {});
    CHECK(contents.find("discarded=130") != std::string::npos);
    release.set_value();
    engine.stop(); media.stop();
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    int occupied_port = 0;
    const int occupied = reserve_port(&occupied_port);
    CHECK(occupied >= 0);
    {
        Child child(argv[1], free_port(), occupied_port);
        CHECK(child.exited(1)); // WS bind failure after the snapshot thread started must not abort.
    }
    {
        Child child(argv[1], occupied_port, free_port());
        CHECK(child.exited(1)); // HTTP bind failure must release WS and snapshot resources.
    }
    {
        Child child(argv[1], free_port(), free_port(), occupied_port);
        CHECK(child.exited(1)); // Hook bind failure is also an ordinary nonzero exit.
    }
    if (occupied >= 0)
        ::close(occupied);
    {
        const auto port = free_port();
        Child child(argv[1], port, free_port());
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(0, 100000);
        bool ready = false;
        for (int i = 0; i < 100; ++i) {
            auto response = client.Get("/api/system/info");
            if (response && response->body.find("\"ready\":true") != std::string::npos) {
                CHECK(response->body.find("\"engine_state\":\"DISCONNECTED\"") !=
                      std::string::npos);
                CHECK(response->body.find("\"engine_write_ready\":false") != std::string::npos);
                CHECK(response->body.find("\"lifecycle_state\":\"RUNNING\"") != std::string::npos);
                CHECK(response->body.find("\"websocket\":{") != std::string::npos);
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(ready);
        if (child.pid > 0)
            ::kill(child.pid, SIGTERM);
        CHECK(child.exited(0));
    }
    terminate_while_waiting_for_ack(argv[1]);
    terminate_during_snapshot(argv[1]);
    return g_fail;
}
