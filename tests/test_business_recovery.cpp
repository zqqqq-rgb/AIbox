#include "db.h"
#include "fake_engine.h"
#include "mock_zlm.h"
#include "test_util.h"
#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <httplib.h>
#include <json/json.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace aibox;
using namespace aibox::biz;
using testutil::FakeEngine;
using testutil::MockZlm;
namespace {
#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error(#condition);                                                  \
    } while (0)
template <class Predicate> bool until(Predicate predicate, int timeout = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
int free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    ::close(fd);
    return ntohs(address.sin_port);
}
Json::Value json(const std::string& text) {
    Json::Value root;
    std::istringstream input(text);
    input >> root;
    return root;
}
struct Process {
    std::string executable, directory, config;
    int port = free_port(), ws_port = free_port();
    pid_t pid = -1;
    Process(const char* binary, int media_port) : executable(binary) {
        char temporary[] = "/tmp/aibox_recovery_XXXXXX";
        const auto created = ::mkdtemp(temporary);
        REQUIRE(created);
        directory = created;
        config = directory + "/config.json";
        Json::Value value;
        value["db_path"] = directory + "/business.db";
        value["uds_path"] = directory + "/engine.sock";
        value["http_port"] = port;
        value["ws_port"] = ws_port;
        value["hook_port"] = free_port();
        value["bind_host"] = "127.0.0.1";
        value["zlm"]["port"] = media_port;
        std::ofstream output(config);
        output << value;
    }
    void start() {
        REQUIRE(pid < 0);
        const int sink = ::open((directory + "/child.log").c_str(),
                                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        REQUIRE(sink >= 0);
        const auto max_fd = ::sysconf(_SC_OPEN_MAX);
        pid = ::fork();
        if (pid == 0) {
            ::dup2(sink, STDOUT_FILENO);
            ::dup2(sink, STDERR_FILENO);
            // A restarted child must not inherit the mock engine's connected sockets.
#ifdef SYS_close_range
            if (::syscall(SYS_close_range, 3U, ~0U, 0) != 0)
#endif
                for (int fd = 3; fd < max_fd; ++fd)
                    ::close(fd);
            ::execl(executable.c_str(), executable.c_str(), "--config", config.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(sink);
        REQUIRE(pid > 0);
    }
    void stop(int signal) {
        REQUIRE(pid > 0);
        REQUIRE(::kill(pid, signal) == 0);
        int status = 0;
        REQUIRE(until([&] { return ::waitpid(pid, &status, WNOHANG) == pid; }, 6000));
        pid = -1;
        REQUIRE(signal == SIGKILL ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                                  : WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    bool ready() {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(0, 100000);
        client.set_read_timeout(1, 0);
        return until([&] {
            const auto response = client.Get("/api/system/info");
            if (!response)
                return false;
            const auto data = json(response->body)["data"];
            return data["ready"].asBool() && data["engine_state"] == "ONLINE";
        });
    }
    ~Process() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        for (const auto* name : {"config.json", "child.log", "engine.sock", "business.db",
                                 "business.db-wal", "business.db-shm"})
            ::unlink((directory + "/" + name).c_str());
        ::rmdir(directory.c_str());
    }
};
struct Gate {
    std::promise<void> arrived, release;
    std::future<void> reached = arrived.get_future();
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> once{false}, opened{false};
    void block() {
        if (!once.exchange(true)) {
            arrived.set_value();
            released.wait();
        }
    }
    void open() {
        if (!opened.exchange(true))
            release.set_value();
    }
};
struct ScopedGate {
    std::shared_ptr<Gate> value = std::make_shared<Gate>();
    ~ScopedGate() {
        value->open();
    }
};
int64_t create(Process& process) {
    httplib::Client client("127.0.0.1", process.port);
    auto response = client.Post(
        "/api/cameras",
        R"({"name":"crash recovery","rtsp_main":"rtsp://camera/main","rtsp_sub":"rtsp://camera/sub"})",
        "application/json");
    REQUIRE(response);
    auto result = json(response->body);
    REQUIRE(result["code"] == 0);
    return result["data"]["id"].asInt64();
}
void history(Db& db, int64_t id) {
    EventRow event;
    event.camera_id = id;
    event.algo_id = "person";
    event.type = "history";
    event.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    REQUIRE(db.insert_event(event) > 0);
    SegmentRow segment;
    segment.camera_id = id;
    segment.file_path = "/test/history.mp4";
    segment.start_ts = event.ts;
    REQUIRE(db.insert_segment(segment) > 0);
}
void crash_stop(const char* binary, bool deleted, int step) {
    fprintf(stderr, "[recovery] %s step=%d\n", deleted ? "delete" : "disable", step);
    MockZlm media;
    FakeEngine engine;
    REQUIRE(media.start());
    Process process(binary, media.port());
    REQUIRE(engine.start(process.directory + "/engine.sock"));
    process.start();
    REQUIRE(process.ready());
    const auto id = create(process);
    Db db(process.directory + "/business.db");
    history(db, id);
    CameraConfigState baseline;
    REQUIRE(db.get_camera_config_state(id, baseline));
    ScopedGate scope;
    auto gate = scope.value;
    if (step < 2)
        engine.command_boundary([=](const std::string& cmd, bool before) {
            if (cmd == "remove_cam" && before == (step == 0))
                gate->block();
        });
    if (step == 2 || step == 3)
        media.after_delete([=](const std::string& key) {
            const auto suffix = step == 2 ? "_main" : "_sub";
            if (key == "__defaultVhost__/live/cam" + std::to_string(id) + suffix)
                gate->block();
        });
    if (step == 4)
        REQUIRE(db.exec("CREATE TRIGGER hold_confirmation BEFORE UPDATE OF applied_revision ON "
                        "camera_config_state "
                        "WHEN NEW.applied_revision > OLD.applied_revision "
                        "BEGIN SELECT RAISE(FAIL, 'test holds final confirmation'); END;"));
    auto request = std::async(std::launch::async, [&] {
        httplib::Client client("127.0.0.1", process.port);
        client.set_read_timeout(3, 0);
        const auto path = "/api/cameras/" + std::to_string(id);
        auto response = deleted
                            ? client.Delete(path.c_str())
                            : client.Put(path.c_str(), "{\"enabled\":false}", "application/json");
        return response ? response->body : "";
    });
    if (step < 4)
        REQUIRE(gate->reached.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    else
        REQUIRE(until([&] {
            return media.source("cam1_main").empty() && media.source("cam1_sub").empty() &&
                   engine.remove_cam > 0;
        }));
    CameraConfigState interrupted;
    REQUIRE(db.get_camera_config_state(id, interrupted));
    REQUIRE(interrupted.desired_revision == baseline.desired_revision + 1);
    REQUIRE(interrupted.applied_revision == baseline.applied_revision);
    REQUIRE((interrupted.deleted_at != 0) == deleted);
    proto::AddCam interrupted_camera;
    proto::SetAlgos interrupted_algos;
    REQUIRE(engine.camera(id, interrupted_camera, interrupted_algos) == (step == 0));
    REQUIRE(media.source("cam1_main").empty() == (step >= 2));
    REQUIRE(media.source("cam1_sub").empty() == (step >= 3));
    process.stop(SIGKILL);
    const auto response = request.get();
    REQUIRE(response.empty() || json(response)["code"] != 0);
    gate->open();
    engine.command_boundary({});
    media.after_delete({});
    if (step == 4)
        REQUIRE(db.exec("DROP TRIGGER hold_confirmation;"));
    process.start();
    REQUIRE(process.ready());
    CameraConfigState recovered;
    REQUIRE(until([&] {
        return db.get_camera_config_state(id, recovered) && recovered.apply_status == "succeeded" &&
               recovered.applied_revision == interrupted.desired_revision;
    }));
    proto::AddCam camera;
    proto::SetAlgos algos;
    REQUIRE(!engine.camera(static_cast<uint32_t>(id), camera, algos));
    REQUIRE(media.source("cam1_main").empty() && media.source("cam1_sub").empty());
    REQUIRE(db.list_events(id, "", 0, 0, 10).size() == 1);
    REQUIRE(db.list_segments(id, 0, 0, 10).size() == 1);
    REQUIRE(db.list_cameras(false).size() == (deleted ? 0U : 1U));
    REQUIRE(db.list_cameras(false, true).size() == 1);
    const auto operations = db.prepare("SELECT count(*) FROM config_operations WHERE camera_id=1 "
                                       "AND target_revision=2 AND status='succeeded';");
    REQUIRE(operations && operations->step() && operations->i64(0) == 1);
    process.stop(SIGTERM);
    engine.stop();
    media.stop();
}

// Minimal socket client: verifies metadata subscription after an actual process restart.
struct Viewer {
    int fd = -1;
    explicit Viewer(int port, int64_t id) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        const auto request =
            "GET /ws/meta/" + std::to_string(id) +
            " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        REQUIRE(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(request.size()));
        std::string reply;
        REQUIRE(until([&] {
            char bytes[1024];
            auto n = ::recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (n > 0)
                reply.append(bytes, static_cast<size_t>(n));
            return reply.find("\r\n\r\n") != std::string::npos;
        }));
        REQUIRE(reply.find("HTTP/1.1 101") == 0);
    }
    bool receives(FakeEngine& engine, uint32_t id) {
        std::string input;
        proto::DetectionsEvt detection;
        detection.id = id;
        detection.ts = 1000;
        detection.frame_seq = 1;
        detection.frame_w = 640;
        detection.frame_h = 480;
        return until([&] {
            engine.emit_meta(id, proto::encode_event(detection));
            char bytes[1024];
            auto n = ::recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (n > 0)
                input.append(bytes, static_cast<size_t>(n));
            return input.find("detections") != std::string::npos;
        });
    }
    ~Viewer() {
        if (fd >= 0)
            ::close(fd);
    }
};
void business_restart(const char* binary) {
    MockZlm media;
    FakeEngine engine;
    REQUIRE(media.start());
    Process process(binary, media.port());
    REQUIRE(engine.start(process.directory + "/engine.sock"));
    process.start();
    REQUIRE(process.ready());
    const auto id = create(process);
    Db db(process.directory + "/business.db");
    proto::RegistryReadyEvt registry;
    proto::RegistryAlgo model;
    model.algo_id = "person";
    model.trigger = "steady";
    model.npu_cost_ms = 100;
    model.npu_mem_kb = 100;
    registry.algos.push_back(model);
    REQUIRE(db.replace_algorithms(registry));
    httplib::Client client("127.0.0.1", process.port);
    const auto response = client.Put(
        ("/api/cameras/" + std::to_string(id) + "/algos").c_str(),
        R"({"algos":[{"algo_id":"person","threshold":0.7,"fps":4,"roi":[[0,0],[1,0],[1,1]],"rule":{"rule_type":"dwell","dwell_sec":12}}]})",
        "application/json");
    REQUIRE(response && json(response->body)["code"] == 0);
    {
        Viewer first(process.ws_port, id);
        REQUIRE(first.receives(engine, id));
    }
    proto::AddCam camera;
    proto::SetAlgos before;
    REQUIRE(engine.camera(id, camera, before));
    REQUIRE(before.algos.size() == 1);
    const auto media_deletes = media.del.load();
    const auto engine_removes = engine.remove_cam.load();
    process.stop(SIGKILL); // Engine and ZLM objects survive; only businessd loses memory.
    process.start();
    REQUIRE(process.ready());
    proto::SetAlgos after;
    REQUIRE(engine.camera(id, camera, after));
    REQUIRE(after.algos.size() == 1 &&
            proto::encode_request(1, after) == proto::encode_request(1, before));
    REQUIRE(after.algos[0].fps == before.algos[0].fps && after.algos[0].roi.size() == 3);
    REQUIRE(after.algos[0].dwell_sec == 12);
    REQUIRE(media.del == media_deletes); // Duplicate engine channel recovery preserves media.
    REQUIRE(engine.remove_cam > engine_removes);
    {
        Viewer second(process.ws_port, id);
        REQUIRE(second.receives(engine, id));
    }
    process.stop(SIGTERM);
    engine.stop();
    media.stop();
}

// The task and both camera targets commit before any IPC. Kill the real businessd
// at deterministic IPC/confirmation boundaries, retaining the engine and media.
void task_crash_recovery(const char* binary, bool enabled, int step) {
    fprintf(stderr, "[task recovery] %s step=%d\n", enabled ? "enable" : "disable", step);
    MockZlm media;
    FakeEngine engine;
    engine.rule_v2(true);
    engine.metrics(R"({"npu":{"steady_cores":[0,1],"triggered_cores":[2]}})");
    REQUIRE(media.start());
    Process process(binary, media.port());
    REQUIRE(engine.start(process.directory + "/engine.sock"));
    process.start();
    REQUIRE(process.ready());
    const auto first = create(process), second = create(process);
    Db db(process.directory + "/business.db");
    proto::RegistryReadyEvt registry;
    proto::RegistryAlgo model;
    model.algo_id = "person";
    model.trigger = "steady";
    model.npu_cost_ms = 20;
    model.npu_mem_kb = 100;
    registry.algos.push_back(model);
    REQUIRE(db.replace_algorithms(registry));
    httplib::Client client("127.0.0.1", process.port);
    client.set_read_timeout(4, 0);
    auto body = json(
        R"({"name":"crash task","camera_ids":[],"algorithms":[{"algo_id":"person","fps":3,"threshold":0.6}],"rules":[{"key":"entry","algo_id":"person","capability_id":"region_in","roi":[[0,0],[1,0],[1,1],[0,1]]}]})");
    body["camera_ids"].append(Json::Int64(first));
    body["camera_ids"].append(Json::Int64(second));
    auto save = [&] {
        const auto response = client.Post("/api/tasks", body.toStyledString(), "application/json");
        REQUIRE(response && json(response->body)["code"] == 0);
        return json(response->body)["data"]["id"].asInt64();
    };
    auto transition = [&](int64_t id, bool value) {
        AlgorithmTask task;
        REQUIRE(db.get_task(id, task));
        Json::Value request;
        request["revision"] = Json::Int64(task.revision);
        request["enabled"] = value;
        const auto response = client.Put(("/api/tasks/" + std::to_string(id) + "/enabled").c_str(),
                                         request.toStyledString(), "application/json");
        REQUIRE(response && json(response->body)["code"] == 0);
    };
    const auto survivor = save();
    transition(survivor, true);
    const auto target = save();
    if (!enabled)
        transition(target, true);
    AlgorithmTask baseline;
    REQUIRE(db.get_task(target, baseline));
    std::vector<CameraConfigState> states;
    for (auto id : {first, second}) {
        CameraConfigState state;
        REQUIRE(db.get_camera_config_state(id, state));
        states.push_back(state);
    }
    const auto media_deletes = media.del.load();
    ScopedGate scope;
    auto gate = scope.value;
    auto commands = std::make_shared<std::atomic<int>>(0);
    if (step < 3)
        engine.command_boundary([=](const std::string& command, bool before) {
            if (command != "set_rules_v2" || before != (step != 1))
                return;
            if (++*commands == (step == 2 ? 2 : 1))
                gate->block();
        });
    else
        REQUIRE(db.exec("CREATE TRIGGER hold_task_confirmation BEFORE UPDATE OF applied_revision "
                        "ON camera_config_state WHEN NEW.applied_revision > OLD.applied_revision "
                        "BEGIN SELECT RAISE(FAIL, 'hold task confirmation'); END;"));
    auto request = std::async(std::launch::async, [&] {
        httplib::Client pending("127.0.0.1", process.port);
        pending.set_read_timeout(3, 0);
        Json::Value value;
        value["revision"] = Json::Int64(baseline.revision);
        value["enabled"] = enabled;
        const auto response =
            pending.Put(("/api/tasks/" + std::to_string(target) + "/enabled").c_str(),
                        value.toStyledString(), "application/json");
        return response ? response->body : "";
    });
    auto has_source = [&](int64_t camera_id, int64_t task_id) {
        proto::AddCam camera;
        proto::SetAlgos config;
        if (!engine.camera(static_cast<uint32_t>(camera_id), camera, config))
            return false;
        for (const auto& algorithm : config.algos)
            for (const auto& rule : algorithm.rules)
                if (rule.task_id == static_cast<uint64_t>(task_id))
                    return true;
        return false;
    };
    if (step < 3)
        REQUIRE(gate->reached.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    else
        REQUIRE(until([&] { return has_source(first, target) == enabled; }));
    AlgorithmTask interrupted;
    REQUIRE(db.get_task(target, interrupted));
    REQUIRE(interrupted.enabled == enabled && interrupted.revision == baseline.revision + 1);
    const auto deployments = db.task_deployments(target);
    REQUIRE(deployments.size() == 2);
    std::vector<std::string> snapshots;
    for (size_t index = 0; index < deployments.size(); ++index) {
        const auto& deployment = deployments[index];
        CameraConfigState state;
        ConfigOperationRow operation;
        REQUIRE(db.get_camera_config_state(deployment.camera_id, state));
        REQUIRE(state.desired_revision == states[index].desired_revision + 1);
        REQUIRE(deployment.task_revision == interrupted.revision);
        REQUIRE(deployment.target_revision == state.desired_revision);
        REQUIRE(db.get_config_operation(deployment.operation_id, operation));
        snapshots.push_back(operation.target_config_json);
        if (index == 1 || step != 2)
            REQUIRE(state.applied_revision == states[index].applied_revision);
        else
            REQUIRE(state.applied_revision == state.desired_revision);
        REQUIRE(has_source(deployment.camera_id, survivor));
        REQUIRE(has_source(deployment.camera_id, target) ==
                ((index == 0 && step != 0) ? enabled : !enabled));
    }
    REQUIRE(!db.task_runtime_settled(target));
    process.stop(SIGKILL);
    const auto response = request.get();
    REQUIRE(response.empty() || json(response)["code"] != 0);
    gate->open();
    engine.command_boundary({});
    if (step == 3)
        REQUIRE(db.exec("DROP TRIGGER hold_task_confirmation;"));
    process.start();
    REQUIRE(process.ready());
    REQUIRE(until([&] { return db.task_runtime_settled(target); }));
    AlgorithmTask recovered;
    REQUIRE(db.get_task(target, recovered));
    REQUIRE(recovered.revision == interrupted.revision && recovered.enabled == enabled);
    for (size_t index = 0; index < deployments.size(); ++index) {
        const auto& deployment = deployments[index];
        ConfigOperationRow operation;
        CameraConfigState state;
        REQUIRE(db.get_config_operation(deployment.operation_id, operation));
        REQUIRE(operation.status == "succeeded" &&
                operation.target_config_json == snapshots[index]);
        REQUIRE(db.get_camera_config_state(deployment.camera_id, state));
        REQUIRE(state.applied_revision == deployment.target_revision);
        REQUIRE(state.desired_revision == deployment.target_revision);
        REQUIRE(has_source(deployment.camera_id, survivor));
        REQUIRE(has_source(deployment.camera_id, target) == enabled);
    }
    REQUIRE(media.del == media_deletes);
    const auto result = client.Get(("/api/tasks/" + std::to_string(target)).c_str());
    REQUIRE(result && json(result->body)["data"]["applied"].asBool());
    REQUIRE(json(result->body)["data"]["status"] == (enabled ? "running" : "disabled"));
    process.stop(SIGTERM);
    engine.stop();
    media.stop();
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[2]) == "--tasks-only") {
        for (bool enabled : {true, false})
            for (int step = 0; step < 4; ++step) {
                try {
                    task_crash_recovery(argv[1], enabled, step);
                } catch (const std::exception& error) {
                    fprintf(stderr, "task recovery %s/%d: %s\n", enabled ? "enable" : "disable",
                            step, error.what());
                    g_fail = 1;
                }
            }
        return g_fail;
    }
    if (argc != 2)
        return 2;
    for (bool deleted : {false, true})
        for (int step = 0; step < 5; ++step) {
            try {
                crash_stop(argv[1], deleted, step);
            } catch (const std::exception& error) {
                fprintf(stderr, "recovery %s/%d: %s\n", deleted ? "delete" : "disable", step,
                        error.what());
                g_fail = 1;
            }
        }
    try {
        business_restart(argv[1]);
    } catch (const std::exception& error) {
        fprintf(stderr, "restart: %s\n", error.what());
        g_fail = 1;
    }
    return g_fail;
}
