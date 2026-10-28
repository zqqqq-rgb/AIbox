// T4.2 单测（§4-T4.2 断言表）：FakeEngine 脚本重放——
//   ① SYNCING 全路径（ensure_proxies / add_cam / set_algos 全 ack → ONLINE）
//   ② SYNCING 部分失败：第 3 台 add_cam 恒败 → 重试×2 共 3 次收到，degraded 继续，终态 ONLINE
//   ③ ONLINE 后断连 → DISCONNECTED → 重连 → 全量重建（新 engine 收到全部 add_cam）
//   ④ ONLINE 后 evt 转发 + 运行期命令 ack 传播 + 心跳保活
// 心跳"超时→断线"与"断连→重连"共用同一 pump/loop 路径，由 ③ 覆盖；
// ⑤ 验证心跳在持续 ack 下保活（get_metrics 计数随时间增长）。
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db.h"
#include "fake_engine.h"
#include "test_util.h"
#include "uds_client.h"

using namespace aibox;
using namespace aibox::biz;
using aibox::testutil::FakeEngine;

namespace {

int g_sock_seq = 0;

std::string make_sock_path() {
    return "/tmp/aibox_t_uds_client_" + std::to_string(::getpid()) + "_" +
           std::to_string(++g_sock_seq) + ".sock";
}

// 轮询直到条件成立（测试用忙等，毫秒粒度）
template <typename Pred>
bool wait_for(Pred p, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

// 板上实测教训：CHECK 失败后继续执行 + 裸下标（lines[0]）会把底层失败放大成
// SegFault——关键前置一律 REQUIRE（失败即 return，带 client 状态）
const char* state_name(UdsClient::State s) {
    switch (s) {
        case UdsClient::State::DISCONNECTED: return "DISCONNECTED";
        case UdsClient::State::SYNCING: return "SYNCING";
        case UdsClient::State::ONLINE: return "ONLINE";
    }
    return "?";
}

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "REQUIRE FAIL %s:%d: %s\n", __FILE__, __LINE__,  \
                    #cond);                                                  \
            g_fail = 1;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

#define REQUIRE_STATE(cli, cond)                                             \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "REQUIRE FAIL %s:%d: %s (state=%s)\n", __FILE__, \
                    __LINE__, #cond, state_name((cli).state()));             \
            g_fail = 1;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

struct Fixture {
    Db db{":memory:"};
    std::vector<int64_t> cam_ids;

    Fixture() {
        CHECK(db.migrate() == Db::kSchemaLatest);
        for (int i = 1; i <= 3; ++i) {
            CameraRow c;
            c.name = "cam" + std::to_string(i);
            c.rtsp_main = "rtsp://127.0.0.1:554/live/cam" + std::to_string(i) + "_main";
            c.rtsp_sub = "rtsp://127.0.0.1:554/live/cam" + std::to_string(i) + "_sub";
            const int64_t id = db.insert_camera(c);
            if (id <= 0) {
                fprintf(stderr, "FATAL fixture insert_camera id=%lld err=%s changes=%d\n",
                        static_cast<long long>(id), db.last_error().c_str(),
                        db.changes());
                g_fail = 1;
                continue;  // 后续断言会因计数不符失败，但根因已在此打印
            }
            cam_ids.push_back(id);
            CHECK(db.set_camera_algos(id, {{"person", 0.5, 3, "", true, "", 1, ""}}));
        }
    }
};

// ① SYNCING 全路径
void test_sync_full_path() {
    Fixture fx;
    FakeEngine eng;
    const std::string path = make_sock_path();
    CHECK(eng.start(path));

    std::atomic<int> proxies{0};
    std::mutex evt_mu;
    std::vector<std::string> evts;
    UdsClient::Hooks hooks;
    hooks.ensure_proxies = [&] { ++proxies; return true; };
    hooks.on_evt = [&](const proto::ParsedMsg& pm, const std::string&) {
        std::lock_guard<std::mutex> lk(evt_mu);
        evts.push_back(pm.name);
    };
    UdsClient cli(path, fx.db, hooks, {/*connect*/ 50, /*ack*/ 1000, /*retry*/ 1,
                                       /*ping*/ 1000, /*fail*/ 3});
    cli.start();
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 5000));

    const auto st = cli.sync_stats();
    CHECK(st.ensure_proxies_calls == 1);
    CHECK(st.add_cam_ok == 3 && st.add_cam_fail == 0);
    CHECK(st.set_algos_ok == 3 && st.set_algos_fail == 0);
    CHECK(st.degraded_cams == 0);
    CHECK(eng.add_cam.load() == 3);
    CHECK(eng.set_algos.load() == 3);
    cli.stop();
    eng.stop();
}

// ② SYNCING 部分失败降级
void test_sync_partial_failure() {
    Fixture fx;
    FakeEngine eng;
    eng.script.fail_add_cam.insert(3);  // 第 3 台恒失败（ack ret=1003）
    const std::string path = make_sock_path();
    CHECK(eng.start(path));

    UdsClient::Hooks hooks;
    UdsClient cli(path, fx.db, hooks, {50, 1000, 2 /*重试×2*/, 1000, 3});
    cli.start();
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 8000));  // degraded 仍终态 ONLINE

    const auto st = cli.sync_stats();
    CHECK(st.add_cam_ok == 2 && st.add_cam_fail == 1);
    CHECK(st.set_algos_ok == 2);          // 失败通道不再 set_algos
    CHECK(st.degraded_cams == 1);
    // cam3 初始 + 一次安全重建 + 重试×2；重建次数有界。
    CHECK(eng.add_cam.load() == 6);
    CHECK(eng.set_algos.load() == 2);
    cli.stop();
    eng.stop();
}

// ③ ONLINE 后断连 → 全量重建
void test_reconnect_full_rebuild() {
    Fixture fx;
    const std::string path = make_sock_path();
    FakeEngine eng1;
    CHECK(eng1.start(path));

    std::atomic<int> proxies{0};
    UdsClient::Hooks hooks;
    hooks.ensure_proxies = [&] { ++proxies; return true; };
    UdsClient cli(path, fx.db, hooks, {100 /*重连间隔*/, 1000, 1, 500, 3});
    cli.start();
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 5000));
    CHECK(eng1.add_cam.load() == 3);

    // 模拟 engine 崩溃：断开全部连接
    eng1.stop();
    REQUIRE_STATE(cli,
                  wait_for([&] { return cli.state() == UdsClient::State::DISCONNECTED; }, 3000));

    // engine 重启（新进程等价：全新 UdsServer + 全新计数器）
    FakeEngine eng2;
    CHECK(eng2.start(path));
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 8000));
    CHECK(wait_for([&] { return eng2.add_cam.load() == 3; }, 3000));  // 全量重建
    CHECK(eng2.set_algos.load() == 3);
    CHECK(proxies.load() == 2);  // 每次 SYNCING 前都重建代理
    cli.stop();
    eng2.stop();
}

// ④ evt 转发 + 运行期命令 + sub_meta
void test_evt_and_runtime_cmds() {
    Fixture fx;
    FakeEngine eng;
    const std::string path = make_sock_path();
    CHECK(eng.start(path));

    std::mutex evt_mu;
    std::vector<std::string> evts;
    std::vector<std::string> lines;
    UdsClient::Hooks hooks;
    hooks.on_evt = [&](const proto::ParsedMsg& pm, const std::string& line) {
        std::lock_guard<std::mutex> lk(evt_mu);
        evts.push_back(pm.name);
        lines.push_back(line);  // 原始行透传（detections 全字段转发依赖）
    };
    UdsClient cli(path, fx.db, hooks, {50, 1000, 1, 1000, 3});
    cli.start();
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 5000));

    // engine → businessd 事件转发
    proto::CamStatusEvt cs;
    cs.id = 1;
    cs.online = true;
    eng.emit(proto::encode_event(cs));
    {
        const bool got = wait_for([&] {
            std::lock_guard<std::mutex> lk(evt_mu);
            return evts.size() == 1;
        }, 3000);
        REQUIRE_STATE(cli, got);
        std::lock_guard<std::mutex> lk(evt_mu);
        REQUIRE(!lines.empty());
        CHECK_MSG(lines[0].find("\"camera_status\"") != std::string::npos,
                  "原始行透传: %s", lines[0].c_str());
    }

    // businessd → engine 运行期命令（HttpApi 口径）：ack 透传
    proto::AddCam ac;
    ac.id = 77;
    ac.rtsp_sub = "rtsp://x";
    const proto::Ack a1 = cli.add_cam(ac);
    CHECK(a1.ret == 0);
    CHECK(eng.add_cam.load() == 4);  // SYNCING 3 + 运行期 1
    CHECK(a1.ack != 0);              // msg_id 回填

    proto::RemoveCam rc;
    rc.id = 77;
    CHECK(cli.remove_cam(rc).ret == 0);
    CHECK(eng.remove_cam.load() == 1);

    proto::ReloadAlgo rl;
    rl.algo = "person";
    CHECK(cli.reload_algo(rl).ret == 0);

    // sub_meta：fire-and-forget 不崩（engine 侧订阅表由 T2.5 uds_server 测试覆盖）
    CHECK(cli.sub_meta(1, true));
    CHECK(cli.sub_meta(1, false));

    // get_metrics：ack.data 透传
    std::string data;
    CHECK(cli.get_metrics(&data, 1000));
    CHECK(data == "{}");
    cli.stop();
    eng.stop();
}

// ⑤ 心跳保活（持续 ack 下不误断）+ 离线命令拒绝
void test_ping_and_offline_cmds() {
    Fixture fx;
    FakeEngine eng;
    const std::string path = make_sock_path();
    CHECK(eng.start(path));

    UdsClient::Hooks hooks;
    UdsClient cli(path, fx.db, hooks, {50, 1000, 1, /*ping*/ 150, /*fail*/ 3});
    cli.start();
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 5000));
    // 1s 内应有 ≥4 次 ping（150ms 间隔），且状态保持 ONLINE（ack 喂活）
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(eng.get_metrics.load() >= 4);  // ≥3 为 SYNCING 后首 ping + 周期 ping
    CHECK(cli.online());

    // engine 下线后：运行期命令立即拒绝（ret=-1，不挂死）
    eng.stop();
    CHECK(wait_for([&] { return !cli.online(); }, 3000));
    proto::AddCam ac;
    ac.id = 9;
    ac.rtsp_sub = "rtsp://x";
    const proto::Ack a = cli.add_cam(ac);
    CHECK(a.ret == -1);
    CHECK(!cli.get_metrics(nullptr, 200));
    cli.stop();
}

void test_business_restart_preserves_full_configuration() {
    Fixture fx;
    CamAlgoRow active;
    active.algo_id = "person";
    active.roi_json = "[[0,0],[1,0],[1,1]]";
    active.rule_json = R"({"rule_type":"dwell","dwell_sec":12,"debounce_sec":7})";
    CamAlgoRow disabled;
    disabled.algo_id = "face";
    disabled.enabled = false;
    REQUIRE(fx.db.set_camera_algos(1, {active, disabled}));
    FakeEngine eng;
    const auto path = make_sock_path();
    REQUIRE(eng.start(path));
    UdsClient::Hooks hooks;
    {
        UdsClient cli(path, fx.db, hooks, {50, 1000, 0, 1000, 3});
        cli.start();
        REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 5000));
        CHECK(cli.sync_stats().degraded_cams == 0);
        cli.stop();
    }
    // Engine memory survives; new business client must remove/add/apply, not accept
    // the duplicate response as evidence of configuration equality.
    UdsClient restarted(path, fx.db, hooks, {50, 1000, 0, 1000, 3});
    restarted.start();
    REQUIRE_STATE(restarted, wait_for([&] { return restarted.online(); }, 5000));
    CHECK(restarted.sync_stats().degraded_cams == 0);
    CHECK(eng.remove_cam.load() == 3);
    proto::AddCam camera;
    proto::SetAlgos algos;
    REQUIRE(eng.camera(1, camera, algos));
    CHECK(camera.rtsp_sub == "rtsp://127.0.0.1:554/live/cam1_sub");
    REQUIRE(algos.algos.size() == 1);
    CHECK(algos.algos[0].algo == "person");
    CHECK(algos.algos[0].roi.size() == 3);
    CHECK(algos.algos[0].rule_type == "dwell");
    CHECK(algos.algos[0].dwell_sec == 12 && algos.algos[0].debounce_sec == 7);
    const auto commands = eng.commands();
    REQUIRE(commands.size() >= 6);
    const std::vector<std::string> tail(commands.begin() + 6, commands.end());
    const std::vector<std::string> expected = {
        "add_cam:1", "remove_cam:1", "add_cam:1", "set_algos:1",
        "add_cam:2", "remove_cam:2", "add_cam:2", "set_algos:2",
        "add_cam:3", "remove_cam:3", "add_cam:3", "set_algos:3"};
    CHECK(tail == expected);
    restarted.stop();
}

void test_bad_persisted_config_is_degraded() {
    Fixture fx;
    CamAlgoRow bad;
    bad.algo_id = "person";
    bad.roi_json = "not json";
    REQUIRE(fx.db.set_camera_algos(1, {bad}));
    REQUIRE(fx.db.set_camera_algos(2, {}));
    FakeEngine eng;
    const auto path = make_sock_path();
    REQUIRE(eng.start(path));
    UdsClient cli(path, fx.db, {}, {50, 1000, 0, 1000, 3});
    cli.start();
    REQUIRE_STATE(cli, wait_for([&] { return cli.online(); }, 5000));
    CHECK(cli.sync_stats().degraded_cams == 1);
    proto::AddCam camera;
    proto::SetAlgos algos;
    CHECK(!eng.camera(1, camera, algos));
    REQUIRE(eng.camera(2, camera, algos));
    CHECK(algos.algos.empty());
    cli.stop();
}

void run() {
    // 构建标记：缺此行 = 板上跑的是陈旧二进制（勿信失败行号）
    fprintf(stderr, "[diag] test_uds_client v2 (fixture-guard + state-REQUIRE)\n");
    test_sync_full_path();
    test_sync_partial_failure();
    test_reconnect_full_rebuild();
    test_evt_and_runtime_cmds();
    test_ping_and_offline_cmds();
    test_business_restart_preserves_full_configuration();
    test_bad_persisted_config_is_degraded();
}

}  // namespace

TEST_MAIN("uds_client")
