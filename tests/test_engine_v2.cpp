#include "db.h"
#include "ipc/UdsServer.h"
#include "mock_zlm.h"
#include "model/AlgoPackage.h"
#include "test_util.h"
#include "uds_client.h"
#include <arpa/inet.h>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <json/json.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace aibox;
using namespace aibox::biz;
namespace fs = std::filesystem;
namespace {
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (0)
template <class F> bool until(F predicate, int timeout = 6000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
int port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    ::close(fd);
    return ntohs(address.sin_port);
}
Json::Value json(const std::string& text) {
    Json::Value value;
    std::istringstream(text) >> value;
    return value;
}
struct Temp {
    std::string path;
    Temp() {
        char pattern[] = "/tmp/aibox_v2_XXXXXX";
        auto result = ::mkdtemp(pattern);
        REQUIRE(result);
        path = result;
    }
    ~Temp() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
struct Child {
    pid_t pid = -1;
    std::string log;
    void start(const std::vector<std::string>& args, const std::string& output) {
        log = output;
        std::vector<char*> argv;
        for (const auto& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        REQUIRE(fd >= 0);
        const auto max_fd = ::sysconf(_SC_OPEN_MAX);
        pid = ::fork();
        if (pid == 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
#ifdef SYS_close_range
            if (::syscall(SYS_close_range, 3U, ~0U, 0) != 0)
#endif
                for (int i = 3; i < max_fd; ++i)
                    ::close(i);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        ::close(fd);
        REQUIRE(pid > 0);
    }
    void stop() {
        REQUIRE(pid > 0);
        ::kill(pid, SIGTERM);
        int status = 0;
        REQUIRE(until([&] { return ::waitpid(pid, &status, WNOHANG) == pid; }));
        pid = -1;
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    ~Child() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        if (g_fail || std::uncaught_exceptions()) {
            std::ifstream in(log);
            fprintf(stderr, "%s:\n%s\n", log.c_str(),
                    std::string(std::istreambuf_iterator<char>(in), {}).c_str());
        }
    }
};
proto::SetAlgos rules() {
    proto::SetAlgos c;
    c.id = 1;
    c.config_version = 2;
    proto::AlgoCfg a;
    a.algo = "person";
    a.thr = .2f;
    a.fps = 10;
    a.has_rules = true;
    for (uint64_t id : {1, 2, 3, 4}) {
        proto::RuleCfg r;
        r.task_id = id;
        r.rule_id = 100 + id;
        r.task_revision = 7;
        r.rule_version = 8;
        r.threshold = id == 3 ? .95f : .6f;
        r.fps = 10;
        r.event_type = "shared_event";
        r.debounce_sec = 30;
        r.roi = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        if (id == 4)
            r.roi = {{.7f, .7f}, {1, .7f}, {1, 1}};
        a.rules.push_back(r);
    }
    c.algos = {a};
    return c;
}
void unsupported_engine() {
    Temp tmp;
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    UdsServer server(0);
    std::atomic<int> commands{0};
    server.on_cmd("set_rules_v2", [&](const auto&) {
        ++commands;
        return proto::Ack{};
    });
    REQUIRE(server.start(tmp.path + "/old.sock"));
    UdsClient client(tmp.path + "/old.sock", db, {}, {10, 200, 0, 10000, 3});
    client.start();
    REQUIRE(until([&] { return client.online(); }));
    CHECK(client.set_algos(rules()).ret == 1003);
    CHECK(commands == 0);
    client.stop();
    server.stop();
}
void pipeline(const char* engine_binary, const char* business_binary) {
    Temp tmp;
    fs::create_directories(tmp.path + "/packages/person");
    std::ofstream(tmp.path + "/packages/person/manifest.json")
        << R"({"algo_id":"person","version":"1.0","platform":")" AIBOX_TARGET_PLATFORM
           R"(","model_file":"model.rknn","input_w":64,"input_h":64,"num_cls":1,"default_threshold":0.45,"npu_cost_ms":20,"core":"steady"})";
    std::ofstream(tmp.path + "/packages/person/model.rknn") << "fake model";
    std::ofstream video(tmp.path + "/video.nv12", std::ios::binary);
    std::string frame(64 * 64 * 3 / 2, '\x80');
    for (int i = 0; i < 200; ++i)
        video.write(frame.data(), frame.size());
    video.close();
    {
        Db db(tmp.path + "/business.db");
        REQUIRE(db.migrate() == Db::kSchemaLatest);
        CameraRow camera;
        camera.name = "V2 pipeline";
        camera.rtsp_main = "rtsp://camera/main";
        camera.rtsp_sub = "rtsp://camera/sub";
        REQUIRE(db.insert_camera(camera) == 1);
    }
    testutil::MockZlm media;
    REQUIRE(media.start());
    Child engine, business;
    engine.start({engine_binary, "--fake", "--video", tmp.path + "/video.nv12", "--packages",
                  tmp.path + "/packages", "--uds", tmp.path + "/engine.sock", "--frame-w", "64",
                  "--frame-h", "64", "--fake-fps", "10"},
                 tmp.path + "/engine.log");
    REQUIRE(until([&] { return fs::exists(tmp.path + "/engine.sock"); }));
    int http_port = port();
    Json::Value cfg;
    cfg["db_path"] = tmp.path + "/business.db";
    cfg["uds_path"] = tmp.path + "/engine.sock";
    cfg["bind_host"] = "127.0.0.1";
    cfg["http_port"] = http_port;
    cfg["ws_port"] = port();
    cfg["hook_port"] = port();
    cfg["zlm"]["port"] = media.port();
    std::ofstream(tmp.path + "/config.json") << cfg;
    business.start({business_binary, "--config", tmp.path + "/config.json"},
                   tmp.path + "/business.log");
    httplib::Client http("127.0.0.1", http_port);
    http.set_connection_timeout(0, 100000);
    http.set_read_timeout(1, 0);
    REQUIRE(until([&] {
        auto r = http.Get("/api/system/info");
        if (!r)
            return false;
        auto value = json(r->body)["data"];
        return value["ready"].asBool() && value["engine_state"] == "ONLINE";
    }));
    // Empty control DB avoids replaying cameras. Production businessd remains the event consumer.
    Db empty(":memory:");
    REQUIRE(empty.migrate() == Db::kSchemaLatest);
    UdsClient control(tmp.path + "/engine.sock", empty, {}, {10, 1000, 0, 10000, 3});
    control.start();
    REQUIRE(until([&] { return control.online(); }));
    std::string caps;
    REQUIRE(control.get_capabilities(&caps));
    CHECK(proto::supports_rule_v2(caps));
    REQUIRE(control.set_algos(rules()).ret == 0);
    Json::Value events;
    REQUIRE(until([&] {
        auto r = http.Get("/api/events?camera_id=1&algo_id=person");
        if (!r)
            return false;
        events = json(r->body)["data"];
        return events.isArray() && events.size() >= 2;
    }));
    CHECK(events.size() == 2);
    std::set<int64_t> tasks;
    for (const auto& event : events) {
        tasks.insert(event["task_id"].asInt64());
        CHECK(event["task_revision"] == 7 && event["rule_version"] == 8);
        CHECK(event["rule_id"].asInt64() == event["task_id"].asInt64() + 100);
        CHECK(event["ts"].asInt64() > 1700000000000LL);
        CHECK(event["targets"].size() == 1);
    }
    CHECK(tasks == std::set<int64_t>({1, 2})); // Same event name did not merge different rules.
    {
        Db durable(tmp.path + "/business.db");
        auto rows = durable.list_events(1, "person", 0, 0, 10);
        CHECK(rows.size() == 2);
        for (const auto& row : rows)
            CHECK(row.rule_version == 8 && row.task_revision == 7);
    }
    proto::SetAlgos clear;
    clear.id = 1;
    clear.config_version = 2;
    REQUIRE(control.set_algos(clear).ret == 0);
    control.stop();
    // Durable task admission goes through the production REST service and real engine.
    auto camera_response=http.Post("/api/cameras",R"({"name":"second","rtsp_main":"rtsp://camera/main2","rtsp_sub":"rtsp://camera/sub2"})","application/json");
    REQUIRE(camera_response && json(camera_response->body)["code"]==0);
    const auto second_camera=json(camera_response->body)["data"]["id"].asInt64();
    std::vector<int64_t> task_ids;
    for(int i=0;i<2;++i) {
        Json::Value body;
        body["name"]="REST task "+std::to_string(i); body["enabled"]=false;
        body["camera_ids"].append(1); body["camera_ids"].append(Json::Int64(second_camera));
        Json::Value algorithm; algorithm["algo_id"]="person"; algorithm["threshold"]=.6; algorithm["fps"]=5;
        body["algorithms"].append(algorithm);
        Json::Value rule; rule["key"]="entry"; rule["algo_id"]="person"; rule["capability_id"]="region_in";
        rule["params"]["event_type"]="rest_pipeline";
        for(auto point:{std::pair<int,int>{0,0},{1,0},{1,1},{0,1}}) {
            Json::Value p(Json::arrayValue); p.append(point.first); p.append(point.second); rule["roi"].append(p);
        }
        body["rules"].append(rule);
        auto created=http.Post("/api/tasks",body.toStyledString(),"application/json");
        REQUIRE(created && json(created->body)["code"]==0);
        const auto task=json(created->body)["data"]["id"].asInt64(); task_ids.push_back(task);
        auto enabled=http.Put(("/api/tasks/"+std::to_string(task)+"/enabled").c_str(),R"({"revision":1,"enabled":true})","application/json");
        REQUIRE(enabled && json(enabled->body)["code"]==0 && json(enabled->body)["data"]["applied"].asBool());
        REQUIRE(json(enabled->body)["data"]["operations"].size()==2);
    }
    auto rest_event_count=[&]() {
        auto response=http.Get("/api/events?algo_id=person"); if(!response) return size_t(0);
        size_t count=0;
        const auto result=json(response->body);
        for(const auto& event:result["data"]) if(event["type"]=="rest_pipeline") {
            CHECK(event["task_revision"]==2 && event["rule_version"]==1); ++count;
        }
        return count;
    };
    REQUIRE(until([&] { return rest_event_count()>=4; }));
    auto running=[&] {
        for(auto task:task_ids) {
            auto response=http.Get(("/api/tasks/"+std::to_string(task)).c_str()); if(!response) return false;
            const auto data=json(response->body)["data"];
            if(!data["applied"].asBool() || data["status"]!="running") return false;
        }
        return true;
    };
    business.stop();
    business.start({business_binary,"--config",tmp.path+"/config.json"},tmp.path+"/business-restart.log");
    REQUIRE(until(running)); // Same engine survives; V2 snapshots are replayed on both cameras.
    engine.stop();
    REQUIRE(until([&] { return !running(); }));
    engine.start({engine_binary,"--fake","--video",tmp.path+"/video.nv12","--packages",tmp.path+"/packages",
                  "--uds",tmp.path+"/engine.sock","--frame-w","64","--frame-h","64","--fake-fps","10"},tmp.path+"/engine-restart.log");
    REQUIRE(until(running)); // Empty engine process is fully rebuilt from durable V2 snapshots.
    for(auto task:task_ids) {
        auto response=http.Put(("/api/tasks/"+std::to_string(task)+"/enabled").c_str(),R"({"revision":2,"enabled":false})","application/json");
        REQUIRE(response && json(response->body)["code"]==0 && json(response->body)["data"]["status"]=="disabled");
    }
    business.stop();
    engine.stop();
    // Persistence must survive an actual process exit and database reopen.
    Db reopened(tmp.path + "/business.db");
    CHECK(reopened.migrate() == Db::kSchemaLatest);
    CHECK(reopened.list_events(1, "person", 0, 0, 100).size() >= 4);
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        unsupported_engine();
        pipeline(argv[1], argv[2]);
    } catch (const std::exception& e) {
        fprintf(stderr, "engine_v2: %s\n", e.what());
        return 1;
    }
    if (!g_fail)
        printf("engine_v2 PASSED\n");
    return g_fail;
}
