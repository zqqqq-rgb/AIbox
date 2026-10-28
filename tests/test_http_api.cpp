// T4.4 单测（§4-T4.4 断言表）：12 条 API 正常+错误路径、3001 带建议文案
// rig = 内存 Db + FakeEngine(UDS) + MockZlm(REST) + HttpApi（httplib 客户端打真实端口）
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <httplib.h>
#include <json/json.h>

#include "fake_engine.h"
#include "http_api.h"
#include "mock_zlm.h"
#include "test_util.h"
#include "uds_client.h"
#include "zlm_client.h"  // Rig 直接构造 ZlmClient（http_api.h 仅前置声明）

using namespace aibox;
using namespace aibox::biz;
using aibox::testutil::FakeEngine;
using aibox::testutil::MockZlm;

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

Json::Value parse_json(const std::string& s) {
    Json::Value root;
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    std::string errs;
    if (!rd->parse(s.data(), s.data() + s.size(), &root, &errs)) {
        fprintf(stderr, "bad json: %s\n%.120s\n", errs.c_str(), s.c_str());
        g_fail = 1;
    }
    return root;
}

struct Rig {
    Db db{":memory:"};
    MockZlm zlm;
    FakeEngine eng;
    std::unique_ptr<UdsClient> uds;
    std::unique_ptr<ZlmClient> zlm_cli;
    std::unique_ptr<HttpApi> api;
    int port = 0;

    Rig() {
        CHECK(db.migrate() == Db::kSchemaLatest);
        // 算法注册表：person 100ms/100MB，face 200ms/200MB（预检算例）
        proto::RegistryReadyEvt evt;
        proto::RegistryAlgo a;
        a.algo_id = "person";
        a.version = "1.0.0";
        a.trigger = "steady";
        a.npu_cost_ms = 100.f;
        a.npu_mem_kb = 102400;
        evt.algos.push_back(a);
        a.algo_id = "face";
        a.npu_cost_ms = 200.f;
        a.npu_mem_kb = 204800;
        evt.algos.push_back(a);
        CHECK(db.replace_algorithms(evt));

        CHECK(zlm.start());
        CHECK(eng.start("/tmp/aibox_t_http_api_" + std::to_string(::getpid()) + ".sock"));
        UdsClient::Hooks hooks;
        uds = std::make_unique<UdsClient>(
            "/tmp/aibox_t_http_api_" + std::to_string(::getpid()) + ".sock", db, hooks,
            UdsClient::Params{50, 1000, 1, 1000, 3});
        uds->start();

        zlm_cli = std::make_unique<ZlmClient>(
            ZlmClient::Params{"127.0.0.1", zlm.port(), "s", 2000});
        port = free_port();
        HttpApi::Deps d;
        d.db = &db;
        d.uds = uds.get();
        d.zlm = zlm_cli.get();
        d.http_port = port;
        d.bind_host = "127.0.0.1";
        d.ws_port = 8081;
        api = std::make_unique<HttpApi>(d);
        CHECK(api->start());
        // 等 UDS ONLINE（POST /api/cameras 依赖 engine ack）
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!uds->online() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(uds->online());
    }

    ~Rig() {
        api->stop();
        uds->stop();
        eng.stop();
        zlm.stop();
    }

    // 已启用的摄像头（直接走 DB/ZLM/engine 完整链路注入）
    int64_t add_camera(const char* name) {
        CameraRow c;
        c.name = name;
        c.rtsp_main = "rtsp://127.0.0.1:554/live/cam9_main";
        c.rtsp_sub = "rtsp://127.0.0.1:554/live/cam9_sub";
        const int64_t id = db.insert_camera(c);
        std::string km, ks;
        zlm_cli->add_camera_proxies(static_cast<int>(id), c.rtsp_main, c.rtsp_sub, &km,
                                    &ks);
        db.set_camera_keys(id, km, ks);
        proto::AddCam ac;
        ac.id = static_cast<uint32_t>(id);
        ac.rtsp_sub = c.rtsp_sub;
        uds->add_cam(ac);
        db.update_camera(c);
        return id;
    }
};

// httplib::Client 含 mutex 不可拷贝（板上版本实测）：各用例直接局部构造
#define MAKE_CLI(var) httplib::Client var("127.0.0.1", r.port)

// ---- 11. system/info ----
void test_system_info() {
    Rig r;
    MAKE_CLI(cli);
    auto res = cli.Get("/api/system/info");
    CHECK(res && res->status == 200);
    const Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    CHECK(root["data"].isMember("version"));
    CHECK(root["data"]["ws_port"].asInt() == 8081);
    CHECK(root["data"]["ready"].asBool() == false);
    r.api->set_ready(true);
    res = cli.Get("/api/system/info");
    CHECK(parse_json(res->body)["data"]["ready"].asBool() == true);
}

// ---- 7. algorithms ----
void test_algorithms() {
    Rig r;
    MAKE_CLI(cli);
    auto res = cli.Get("/api/algorithms");
    CHECK(res && res->status == 200);
    const Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    const Json::Value& arr = root["data"];
    CHECK(arr.isArray() && arr.size() == 2);
    CHECK(arr[0]["algo_id"].asString() == "face");
    CHECK(arr[0]["npu_cost_ms"].asDouble() == 200.0);
    CHECK(arr[1]["algo_id"].asString() == "person");
}

// ---- 1/2/3/4 cameras CRUD ----
void test_cameras_crud() {
    Rig r;
    MAKE_CLI(cli);
    // POST 成功
    auto res = cli.Post("/api/cameras",
                        R"({"name":"前门","rtsp_main":"rtsp://m","rtsp_sub":"rtsp://s"})",
                        "application/json");
    CHECK(res && res->status == 200);
    Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    const int id = root["data"]["id"].asInt();
    CHECK(id > 0);
    CHECK(r.zlm.add_main.load() == 1 && r.zlm.add_sub.load() == 1);
    CHECK(r.eng.add_cam.load() == 1);
    // DB 里 keys 已回填
    CameraRow cam;
    CHECK(r.db.get_camera(id, cam));
    CHECK(!cam.zlm_key_main.empty() && cam.enabled);

    // POST 缺字段 → 1001
    res = cli.Post("/api/cameras", R"({"name":"x"})", "application/json");
    CHECK(parse_json(res->body)["code"].asInt() == 1001);

    // GET list
    res = cli.Get("/api/cameras");
    root = parse_json(res->body);
    CHECK(root["data"].isArray() && root["data"].size() == 1);
    CHECK(root["data"][0]["name"].asString() == "前门");

    // PUT 改名（板上 httplib 的 path 参数仅收 const char*：拼接路径须 c_str()）
    const std::string cam_path = "/api/cameras/" + std::to_string(id);
    res = cli.Put(cam_path.c_str(), R"({"name":"后门"})", "application/json");
    CHECK(parse_json(res->body)["code"].asInt() == 0);
    CHECK(r.db.get_camera(id, cam) && cam.name == "后门");

    // PUT 改 rtsp → 旧代理删除 + 新代理创建 + engine 侧 remove+add 重建（详设 §5.4-3）
    const int del_before = r.zlm.del.load();
    res = cli.Put(cam_path.c_str(), R"({"rtsp_sub":"rtsp://s2"})", "application/json");
    CHECK(parse_json(res->body)["code"].asInt() == 0);
    CHECK(r.zlm.del.load() >= del_before + 1);
    CHECK(r.zlm.add_sub.load() == 2);   // 新 sub 重建
    CHECK(r.eng.add_cam.load() == 2);   // POST + rtsp 重建

    // DELETE → 逻辑删 + 代理删除 + engine remove（POST 的 1 次 + rtsp 重建的 1 次）
    res = cli.Delete(cam_path.c_str());
    CHECK(parse_json(res->body)["code"].asInt() == 0);
    CHECK(r.db.get_camera(id, cam) && !cam.enabled);
    CHECK(r.eng.remove_cam.load() == 2);
    // 删除后 list 为空
    res = cli.Get("/api/cameras");
    CHECK(parse_json(res->body)["data"].size() == 0);
}

// ---- 5. algos 预检（§4-T4.4 断言：3001 带 "fps 5→3" 型建议）----
void test_algos_precheck() {
    Rig r;
    const int64_t cam1 = r.add_camera("c1");
    MAKE_CLI(cli);

    // 正常：person fps3 → compute = 100×3/1600 = 18.75%
    const std::string cam1_path = "/api/cameras/" + std::to_string(cam1);
    auto res = cli.Put(cam1_path.c_str(),
                       R"({"algos":[{"algo_id":"person","fps":3,"threshold":0.5}]})",
                       "application/json");
    Json::Value root = parse_json(res->body);
    CHECK_MSG(root["code"].asInt() == 0, "PUT person 实际应答: %s", res->body.c_str());
    CHECK_MSG(root["data"]["npu_est"].isMember("compute_pct"), "缺 npu_est: %s",
              res->body.c_str());
    CHECK(r.db.list_camera_algos(cam1).size() == 1);  // DB 落库
    CHECK(r.eng.set_algos.load() == 1);               // engine 已收到

    // 超限：face fps25 → (100×3 + 200×25)/1600 = 331% → 3001 + 建议
    const int64_t cam2 = r.add_camera("c2");
    const std::string cam2_path = "/api/cameras/" + std::to_string(cam2);
    res = cli.Put(cam2_path.c_str(), R"({"algos":[{"algo_id":"face","fps":25}]})",
                  "application/json");
    root = parse_json(res->body);
    CHECK_MSG(root["code"].asInt() == 3001, "PUT face 实际应答: %s", res->body.c_str());
    const std::string msg = root["msg"].asString();
    CHECK_MSG(msg.find("cam2") != std::string::npos, "建议含通道: %s", msg.c_str());
    CHECK_MSG(msg.find("fps") != std::string::npos, "建议含 fps: %s", msg.c_str());
    CHECK_MSG(msg.find("→") != std::string::npos, "建议含降档箭头: %s", msg.c_str());
    // 被拒绝的配置不落库
    CHECK(r.db.list_camera_algos(cam2).empty());

    // 未注册算法 → 1002（预检前置拦截）
    res = cli.Put(cam2_path.c_str(), R"({"algos":[{"algo_id":"nope","fps":3}]})",
                  "application/json");
    CHECK_MSG(parse_json(res->body)["code"].asInt() == 1002, "PUT nope 实际应答: %s",
              res->body.c_str());

    // fps 越界 → 1001
    res = cli.Put(cam2_path.c_str(), R"({"algos":[{"algo_id":"person","fps":30}]})",
                  "application/json");
    CHECK_MSG(parse_json(res->body)["code"].asInt() == 1001, "PUT fps30 实际应答: %s",
              res->body.c_str());
}

// ---- 8. recordings ----
void test_recordings() {
    Rig r;
    for (int64_t ts : {2000, 1000}) {
        SegmentRow s;
        s.camera_id = 1;
        s.file_path = "/data/mp4/20260101_" + std::to_string(ts) + ".mp4";
        s.start_ts = ts;
        s.duration_s = 300;
        CHECK(r.db.insert_segment(s) > 0);
    }
    MAKE_CLI(cli);
    auto res = cli.Get("/api/recordings?camera_id=1&limit=10");
    const Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    CHECK(root["data"]["total"].asInt() == 2);
    const Json::Value& items = root["data"]["items"];
    CHECK(items[0]["start_ts"].asInt64() == 2000);  // DESC
    CHECK_MSG(items[0]["url"].asString().find("/recordings/20260101_2000.mp4") !=
                  std::string::npos,
              "url=%s", items[0]["url"].asString().c_str());
}

// ---- 9. events ----
void test_events() {
    Rig r;
    EventRow e;
    e.camera_id = 1;
    e.algo_id = "person";
    e.type = "REGION_IN";
    e.ts = 5000;
    e.targets_json = R"([{"cls":0}])";
    CHECK(r.db.insert_event(e) > 0);
    e.algo_id = "face";
    e.ts = 6000;
    CHECK(r.db.insert_event(e) > 0);

    MAKE_CLI(cli);
    auto res = cli.Get("/api/events?camera_id=1&limit=10");
    CHECK(parse_json(res->body)["data"].size() == 2);
    res = cli.Get("/api/events?camera_id=1&algo_id=person");
    const Json::Value arr = parse_json(res->body)["data"];
    CHECK(arr.size() == 1 && arr[0]["type"].asString() == "REGION_IN");
    CHECK(arr[0]["targets"][0]["cls"].asInt() == 0);  // targets_json 反序列化
}

// ---- 10. resources（get_metrics 透传）----
void test_resources() {
    Rig r;
    MAKE_CLI(cli);
    auto res = cli.Get("/api/resources");
    const Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    CHECK(root["data"]["engine_online"].asBool() == true);  // FakeEngine ack data={}
}

// ---- 12. rollback ----
void test_rollback() {
    Rig r;
    MAKE_CLI(cli);
    auto res = cli.Post("/api/algorithms/person/rollback", "{}", "application/json");
    const Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    CHECK(r.eng.reload.load() == 1);
}

// ---- 6. stream ----
void test_stream() {
    Rig r;
    MAKE_CLI(cli);
    auto res = cli.Get("/api/cameras/1/stream?quality=main");
    const Json::Value root = parse_json(res->body);
    CHECK(root["code"].asInt() == 0);
    CHECK(root["data"]["stream"].asString() == "cam1_main");
    CHECK_MSG(root["data"]["signaling"].asString().find("/index/api/webrtc") !=
                  std::string::npos,
              "signaling=%s", root["data"]["signaling"].asString().c_str());
    // 非法 quality → 1001
    res = cli.Get("/api/cameras/1/stream?quality=h265");
    CHECK(parse_json(res->body)["code"].asInt() == 1001);
}

void test_algorithm_validation_and_disable() {
    Rig r;
    const auto id = r.add_camera("validated");
    const std::string path = "/api/cameras/" + std::to_string(id) + "/algos";
    MAKE_CLI(cli);
    auto res = cli.Put(path.c_str(), R"({"algos":[{"algo_id":"person","threshold":0.7,"fps":3,"roi":[[0,0],[1,0],[1,1]],"rule":{"rule_type":"dwell","dwell_sec":12}},{"algo_id":"uninstalled","enabled":false}]})", "application/json");
    CHECK(res && parse_json(res->body)["code"].asInt() == 0);
    proto::AddCam camera;
    proto::SetAlgos applied;
    CHECK(r.eng.camera(static_cast<uint32_t>(id), camera, applied));
    CHECK(applied.algos.size() == 1);
    CHECK(r.db.list_camera_algos(id).size() == 2);
    const int before = r.eng.set_algos.load();
    for (const auto* body : {
             R"({"algos":[{"algo_id":"person","threshold":1.5}]})",
             R"({"algos":[{"algo_id":"person","threshold":"bad"}]})",
             R"({"algos":[{"algo_id":"person","roi":[[0,0],[2,0],[1,1]]}]})",
             R"({"algos":[{"algo_id":"person","roi":"bad"}]})",
             R"({"algos":[{"algo_id":"person","rule":{"rule_type":"bad"}}]})",
             R"({"algos":[{"algo_id":"person"},{"algo_id":"person"}]})"}) {
        res = cli.Put(path.c_str(), body, "application/json");
        CHECK(res && parse_json(res->body)["code"].asInt() == 1001);
        CHECK(r.eng.set_algos.load() == before);
        CHECK(r.db.list_camera_algos(id).size() == 2);
    }
    res = cli.Put(path.c_str(), R"({"algos":[]})", "application/json");
    CHECK(res && parse_json(res->body)["code"].asInt() == 0);
    CHECK(r.eng.camera(static_cast<uint32_t>(id), camera, applied));
    CHECK(applied.algos.empty());
    CHECK(r.db.list_camera_algos(id).empty());
}

void test_camera_rebuild_restores_algorithms() {
    Rig r;
    const auto id = r.add_camera("rebuild");
    const std::string path = "/api/cameras/" + std::to_string(id);
    MAKE_CLI(cli);
    auto res = cli.Put((path + "/algos").c_str(), R"({"algos":[{"algo_id":"person","threshold":0.7,"fps":3,"roi":[[0,0],[1,0],[1,1]],"rule":{"rule_type":"dwell","dwell_sec":12}}]})", "application/json");
    CHECK(res && parse_json(res->body)["code"].asInt() == 0);
    res = cli.Put(path.c_str(), R"({"rtsp_sub":"rtsp://new-source/sub"})", "application/json");
    CHECK(res && parse_json(res->body)["code"].asInt() == 0);
    proto::AddCam camera;
    proto::SetAlgos applied;
    CHECK(r.eng.camera(static_cast<uint32_t>(id), camera, applied));
    CHECK(camera.rtsp_sub == "rtsp://new-source/sub");
    CHECK(applied.algos.size() == 1);
    if (!applied.algos.empty()) {
        CHECK(applied.algos[0].has_roi && applied.algos[0].roi.size() == 3);
        CHECK(applied.algos[0].rule_type == "dwell" && applied.algos[0].dwell_sec == 12);
    }
    const int before = r.eng.add_cam.load();
    res = cli.Put(path.c_str(), R"({"name":"renamed"})", "application/json");
    CHECK(res && parse_json(res->body)["code"].asInt() == 0);
    CHECK(r.eng.add_cam.load() == before);
}

void run() {
    // 构建标记：缺此行 = 板上跑的是陈旧二进制（勿信失败行号）
    fprintf(stderr, "[diag] test_http_api v4 (body-dispatch + put-entry-log)\n");
    test_system_info();
    test_algorithms();
    test_cameras_crud();
    test_algos_precheck();
    test_recordings();
    test_events();
    test_resources();
    test_rollback();
    test_stream();
    test_algorithm_validation_and_disable();
    test_camera_rebuild_restores_algorithms();
}

}  // namespace

TEST_MAIN("http_api")
