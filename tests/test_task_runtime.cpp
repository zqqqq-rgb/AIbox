#include "camera_service.h"
#include "fake_engine.h"
#include "mock_zlm.h"
#include "task_service.h"
#include "test_util.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unistd.h>
using namespace aibox;
using namespace aibox::biz;
namespace {
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (0)
template <class F> bool until(F predicate, int timeout = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
Json::Value draft(int fps = 3) {
    Json::Value body;
    body["name"] = "runtime";
    body["enabled"] = false;
    body["camera_ids"] = Json::Value(Json::arrayValue);
    body["camera_ids"].append(1);
    body["camera_ids"].append(2);
    Json::Value a;
    a["algo_id"] = "person";
    a["fps"] = fps;
    a["threshold"] = .6;
    body["algorithms"].append(a);
    Json::Value r;
    r["key"] = "entry";
    r["algo_id"] = "person";
    r["capability_id"] = "region_in";
    for (auto point : {std::pair<int, int>{0, 0}, {1, 0}, {1, 1}, {0, 1}}) {
        Json::Value p(Json::arrayValue);
        p.append(point.first);
        p.append(point.second);
        r["roi"].append(p);
    }
    body["rules"].append(r);
    return body;
}
struct Rig {
    Db db{":memory:"};
    testutil::FakeEngine engine;
    testutil::MockZlm media;
    std::unique_ptr<ZlmClient> zlm;
    std::unique_ptr<UdsClient> uds;
    std::unique_ptr<CameraService> cameras;
    std::unique_ptr<TaskService> tasks;
    std::atomic<int64_t> clock{1793581140000LL}; // Monday 2026-11-02 00:59 UTC.
    explicit Rig(int wait = 500) {
        REQUIRE(db.migrate() == Db::kSchemaLatest);
        for (int i = 1; i <= 2; ++i) {
            CameraRow c;
            c.name = "camera";
            c.rtsp_main = "rtsp://camera/main";
            c.rtsp_sub = "rtsp://camera/sub";
            REQUIRE(db.insert_camera(c) == i);
        }
        registry(20);
        engine.rule_v2(true);
        engine.metrics(R"({"npu":{"steady_cores":[0,1],"triggered_cores":[2]}})");
        static int seq = 0;
        const auto socket = "/tmp/aibox_task_runtime_" + std::to_string(::getpid()) + "_" +
                            std::to_string(++seq) + ".sock";
        REQUIRE(engine.start(socket));
        REQUIRE(media.start());
        zlm =
            std::make_unique<ZlmClient>(ZlmClient::Params{"127.0.0.1", media.port(), "test", 300});
        UdsClient::Hooks hooks;
        hooks.prepare_camera = [&](const CameraConfigSnapshot& target) {
            return reconcile_camera_media(db, *zlm, target);
        };
        UdsClient::Params up{10, 200, 0, 10000, 3};
        up.schedule_now = [&] { return clock.load(); };
        uds = std::make_unique<UdsClient>(socket, db, hooks, up);
        uds->start();
        REQUIRE(until([&] { return uds->online(); }));
        CameraService::Params p;
        p.apply_wait_ms = wait;
        p.reconciler.retry_ms = 30;
        p.schedule_poll_ms = 20;
        p.schedule_now = [&] { return clock.load(); };
        cameras = std::make_unique<CameraService>(db, *uds, *zlm, p);
        REQUIRE(cameras->start());
        tasks = std::make_unique<TaskService>(db, 2, 2097152, cameras.get());
    }
    ~Rig() {
        if (cameras)
            cameras->stop();
        if (uds)
            uds->stop();
    }
    void registry(float cost) {
        proto::RegistryReadyEvt r;
        proto::RegistryAlgo a;
        a.algo_id = "person";
        a.trigger = "steady";
        a.npu_cost_ms = cost;
        a.npu_mem_kb = 1000;
        r.algos = {a};
        REQUIRE(db.replace_algorithms(r));
    }
    int64_t create(const Json::Value& body = draft()) {
        auto r = tasks->save(0, body);
        REQUIRE(r.code == 0);
        return r.data["id"].asInt64();
    }
    TaskResult enabled(int64_t id, bool value, int64_t revision = 0) {
        if (!revision) {
            AlgorithmTask t;
            REQUIRE(db.get_task(id, t));
            revision = t.revision;
        }
        Json::Value b;
        b["revision"] = Json::Int64(revision);
        b["enabled"] = value;
        return tasks->set_enabled(id, b);
    }
    proto::SetAlgos config(int camera) {
        proto::AddCam c;
        proto::SetAlgos a;
        REQUIRE(engine.camera(camera, c, a));
        return a;
    }
    bool source(int camera, int64_t task) {
        for (const auto& a : config(camera).algos)
            for (const auto& r : a.rules)
                if (r.task_id == uint64_t(task))
                    return true;
        return false;
    }
};
void lifecycle_and_camera_merge() {
    Rig rig;
    auto first = rig.create(), second = rig.create(draft(7));
    auto result = rig.enabled(first, true);
    REQUIRE(result.code == 0 && result.data["accepted"].asBool() &&
            result.data["applied"].asBool());
    REQUIRE(result.data["operations"].size() == 2);
    REQUIRE(rig.source(1, first) && rig.source(2, first));
    REQUIRE(rig.db.task_deployments(first).size() == 2);
    CHECK(rig.enabled(first, true, 1).code == 1003); // Stale CAS cannot create another operation.
    const auto revision = result.data["revision"].asInt64();
    REQUIRE(rig.enabled(first, true, revision).code == 0); // Same-state retry is idempotent.
    AlgorithmTask task;
    REQUIRE(rig.db.get_task(first, task));
    CHECK(task.revision == revision);
    Json::Value remove;
    remove["revision"] = Json::Int64(revision);
    CHECK(rig.tasks->remove(first, remove).code == 1003);
    auto edit = draft();
    edit["revision"] = Json::Int64(revision);
    CHECK(rig.tasks->save(first, edit).code == 1003);
    task.enabled = false;
    CHECK(!rig.db.save_task(task, revision)); // Direct persistence guard too.
    REQUIRE(rig.enabled(second, true).code == 0);
    CHECK(rig.config(1).algos[0].fps == 7 && rig.source(1, first) && rig.source(1, second));
    CamAlgoRow legacy;
    legacy.algo_id = "person";
    legacy.threshold = .3;
    legacy.detect_fps = 2;
    legacy.roi_json = "[[0,0],[1,0],[1,1]]";
    REQUIRE(rig.cameras->set_algos(1, {legacy}).code == 0);
    CHECK(rig.config(1).algos[0].thr == .3f && rig.source(1, first) && rig.source(1, second));
    for (const auto& a : rig.config(1).algos)
        for (const auto& r : a.rules)
            if (r.task_id == uint64_t(first))
                CHECK(r.task_revision == uint64_t(revision));
    CameraPatch patch;
    patch.enabled = false;
    REQUIRE(rig.cameras->update(1, patch).code == 0);
    CHECK(rig.tasks->get(first).data["status"] == "partially_suspended");
    patch.enabled = true;
    patch.rtsp_sub = "rtsp://camera/new_sub";
    REQUIRE(rig.cameras->update(1, patch).code == 0);
    CHECK(rig.source(1, first) && rig.source(1, second));
    REQUIRE(rig.enabled(first, false).code == 0);
    CHECK(!rig.source(1, first) && rig.source(1, second) && !rig.source(2, first));
    REQUIRE(rig.enabled(second, false).code == 0);
    CHECK(rig.config(1).config_version == 1 && rig.config(1).algos[0].fps == 2);
    CHECK(rig.config(2).algos.empty());
    REQUIRE(rig.db.get_task(first, task));
    edit["revision"] = Json::Int64(task.revision);
    REQUIRE(rig.tasks->save(first, edit).code == 0);
    const auto revised = rig.tasks->get(first).data;
    CHECK(revised["status"] == "draft" && !revised["applied"].asBool());
    CHECK(revised["deployments"].empty() && rig.db.task_deployments(first).empty());
    CHECK(!rig.source(1, first) &&
          !rig.source(2, first)); // Saving a draft never applies its rules.
}
void admission_and_atomicity() {
    Rig rig;
    const auto id = rig.create();
    CameraConfigState before;
    REQUIRE(rig.db.get_camera_config_state(1, before));
    rig.engine.rule_v2(false);
    auto result = rig.enabled(id, true);
    CHECK(result.code == 1003 && !result.data["accepted"].asBool());
    rig.engine.rule_v2(true);
    rig.engine.metrics("{}");
    CHECK(rig.enabled(id, true).code == 1003);
    rig.engine.metrics(R"({"npu":{"steady_cores":[0],"triggered_cores":[0]}})");
    CHECK(rig.enabled(id, true).code == 1003);
    rig.engine.metrics(R"({"npu":{"steady_cores":[0,1],"triggered_cores":[2]}})");
    rig.registry(1000);
    CHECK(rig.enabled(id, true).code == 3001);
    rig.registry(20);
    REQUIRE(rig.db.exec("CREATE TRIGGER fail_second_camera BEFORE INSERT ON config_operations WHEN "
                        "NEW.camera_id=2 BEGIN SELECT RAISE(ABORT,'fail'); END;"));
    result = rig.enabled(id, true);
    CHECK(result.code == 5001 && !result.data["accepted"].asBool());
    AlgorithmTask task;
    REQUIRE(rig.db.get_task(id, task));
    CHECK(!task.enabled && task.revision == 1);
    CameraConfigState after;
    REQUIRE(rig.db.get_camera_config_state(1, after));
    CHECK(after.desired_revision == before.desired_revision);
    CHECK(rig.db.task_deployments(id).empty() && !rig.source(1, id));
    REQUIRE(rig.db.exec("DROP TRIGGER fail_second_camera;"));
    REQUIRE(rig.enabled(id, true).code == 0);
    rig.uds->stop();
    result = rig.enabled(id, false);
    CHECK(result.code == 1003 && !result.data["accepted"].asBool());
    CHECK(!rig.tasks->get(id).data["applied"].asBool());
}
void scheduled_boundaries_and_recovery() {
    Rig rig;
    const auto survivor = rig.create();
    REQUIRE(rig.enabled(survivor, true).code == 0);
    auto body = draft(7);
    Json::Value schedule;
    schedule["timezone"] = "UTC";
    schedule["weekdays"] = 1;
    schedule["start_minute"] = 60;
    schedule["end_minute"] = 120;
    body["schedules"].append(schedule);
    const auto id = rig.create(body);
    REQUIRE(rig.enabled(id, true).code == 0);
    REQUIRE(!rig.source(1, id) && !rig.source(2, id));
    REQUIRE(rig.tasks->get(id).data["status"] == "waiting_schedule");
    const auto before = rig.db.task_deployments(id);
    rig.clock += 60000; // Inclusive start at 01:00.
    REQUIRE(until(
        [&] { return rig.source(1, id) && rig.source(2, id) && rig.db.task_runtime_settled(id); }));
    CHECK(rig.config(1).algos[0].fps == 7 && rig.source(1, survivor));
    const auto active = rig.db.task_deployments(id);
    REQUIRE(active.size() == 2 && active[0].target_revision == before[0].target_revision + 1);
    for (int i = 0; i < 5; ++i)
        REQUIRE(rig.db.refresh_task_schedules(rig.clock));
    CHECK(rig.db.task_deployments(id)[0].target_revision == active[0].target_revision);
    // Failure on camera 2 rolls back camera 1 and all deployment links.
    REQUIRE(rig.db.exec("CREATE TRIGGER fail_window BEFORE INSERT ON config_operations WHEN "
                        "NEW.camera_id=2 BEGIN SELECT RAISE(ABORT,'hold'); END;"));
    rig.clock += 3600000; // Exclusive end at 02:00.
    REQUIRE(!rig.db.refresh_task_schedules(rig.clock));
    CHECK(rig.db.task_deployments(id)[0].target_revision == active[0].target_revision);
    CHECK(rig.source(1, id) && rig.source(2, id));
    CHECK(!rig.tasks->get(id).data["applied"].asBool());
    REQUIRE(rig.db.exec("DROP TRIGGER fail_window;"));
    REQUIRE(until([&] {
        return !rig.source(1, id) && !rig.source(2, id) && rig.db.task_runtime_settled(id);
    }));
    REQUIRE(rig.tasks->get(id).data["status"] == "waiting_schedule");
    CHECK(rig.source(1, survivor) && rig.config(1).algos[0].fps == 3);
    // Clock rollback re-enters the window. Reconnect must expire it before replay.
    rig.clock -= 3600000;
    REQUIRE(until(
        [&] { return rig.source(1, id) && rig.source(2, id) && rig.db.task_runtime_settled(id); }));
    auto epoch = rig.uds->connection_epoch();
    rig.clock += 3600000;
    rig.engine.reset_state();
    rig.engine.disconnect_clients();
    REQUIRE(until([&] {
        return rig.uds->online() && rig.uds->connection_epoch() > epoch &&
               rig.db.task_runtime_settled(id);
    }));
    CHECK(!rig.source(1, id) && !rig.source(2, id) && rig.source(1, survivor));
    AlgorithmTask task;
    REQUIRE(rig.db.get_task(id, task));
    CHECK(task.enabled && task.revision == 2); // A clock boundary is not a user edit.
    REQUIRE(rig.enabled(id, false).code == 0);
    rig.clock -= 3600000;
    REQUIRE(rig.db.refresh_task_schedules(rig.clock));
    CHECK(!rig.source(1, id)); // Manual disable takes priority over the schedule.
}
void partial_apply_retry_and_replay() {
    Rig rig(40);
    const auto id = rig.create();
    rig.engine.fail_algorithms(2, true);
    auto result = rig.enabled(id, true);
    REQUIRE(result.code == 1003 && result.data["accepted"].asBool());
    REQUIRE(rig.db.task_deployments(id).size() == 2);
    REQUIRE(until([&] { return rig.source(1, id); }));
    CHECK(!rig.source(2, id) && !rig.tasks->get(id).data["applied"].asBool());
    AlgorithmTask task;
    REQUIRE(rig.db.get_task(id, task));
    CHECK(task.enabled && task.revision == 2);
    rig.engine.fail_algorithms(2, false);
    REQUIRE(until([&] { return rig.db.task_runtime_settled(id); }));
    CHECK(rig.source(2, id));
    // Replay uses the immutable V2 snapshot. An old engine may not downgrade it to V1.
    const auto epoch = rig.uds->connection_epoch();
    rig.engine.rule_v2(false);
    rig.engine.reset_state();
    rig.engine.disconnect_clients();
    REQUIRE(until([&] { return rig.uds->online() && rig.uds->connection_epoch() > epoch; }));
    CHECK(rig.uds->sync_stats().degraded_cams == 2);
    CHECK(!rig.tasks->get(id).data["applied"].asBool());
    rig.engine.rule_v2(true);
    rig.engine.disconnect_clients();
    REQUIRE(until([&] { return rig.uds->online() && rig.db.task_runtime_settled(id); }));
    CHECK(rig.source(1, id) && rig.source(2, id));
    rig.engine.fail_algorithms(2, true);
    result = rig.enabled(id, false);
    REQUIRE(result.data["accepted"].asBool());
    REQUIRE(rig.db.get_task(id, task));
    auto edit = draft();
    edit["revision"] = Json::Int64(task.revision);
    CHECK(rig.tasks->save(id, edit).code == 1003);
    Json::Value remove;
    remove["revision"] = Json::Int64(task.revision);
    CHECK(rig.tasks->remove(id, remove).code == 1003);
    rig.engine.fail_algorithms(2, false);
    REQUIRE(until([&] { return rig.db.task_runtime_settled(id); }));
    REQUIRE(rig.tasks->remove(id, remove).code == 0);
}
void scheduled_peak_and_overnight() {
    Rig rig;
    auto body = draft(7);
    Json::Value night;
    night["timezone"] = "UTC+08:00";
    night["weekdays"] = 1;
    night["start_minute"] = 1320;
    night["end_minute"] = 120;
    body["schedules"].append(night);
    night["start_minute"] = 1380;
    night["end_minute"] = 180;
    body["schedules"].append(night);
    const auto id = rig.create(body);
    rig.registry(1000);
    CHECK(rig.enabled(id, true).code == 3001); // Even outside the current window, reserve peak FPS.
    rig.registry(20);
    REQUIRE(rig.enabled(id, true).code == 0);
    REQUIRE(!rig.source(1, id));
    // Monday 22:00 in UTC+08:00, then Tuesday 02:00: second window still owns Tuesday.
    rig.clock = 1793628000000LL;
    REQUIRE(until(
        [&] { return rig.source(1, id) && rig.source(2, id) && rig.db.task_runtime_settled(id); }));
    const auto active = rig.db.task_deployments(id);
    rig.clock += 4 * 3600000LL;
    REQUIRE(rig.db.refresh_task_schedules(rig.clock));
    CHECK(rig.source(1, id) &&
          rig.db.task_deployments(id)[0].target_revision == active[0].target_revision);
    rig.clock += 3600000LL; // Tuesday 03:00, both windows have ended.
    REQUIRE(until([&] {
        return !rig.source(1, id) && !rig.source(2, id) && rig.db.task_runtime_settled(id);
    }));
    CHECK(rig.tasks->get(id).data["status"] == "waiting_schedule");
}
void schedule_and_deleted_camera() {
    Rig rig;
    auto body = draft();
    Json::Value s;
    s["timezone"] = "UTC";
    s["weekdays"] = 127;
    s["start_minute"] = 0;
    s["end_minute"] = 1440;
    body["schedules"].append(s);
    auto scheduled = rig.create(body);
    auto result = rig.enabled(scheduled, true);
    REQUIRE(result.code == 0 && result.data["accepted"].asBool());
    REQUIRE(rig.enabled(scheduled, false).code == 0);
    const auto id = rig.create();
    REQUIRE(rig.enabled(id, true).code == 0);
    REQUIRE(rig.cameras->remove(1).code == 0);
    CHECK(rig.tasks->get(id).data["status"] == "partially_suspended");
    REQUIRE(rig.enabled(id, false).code == 0);
    CHECK(rig.enabled(id, true).code == 1002); // Camera deletion cannot be undone by a task.
}
} // namespace
int main() {
    try {
        lifecycle_and_camera_merge();
        admission_and_atomicity();
        partial_apply_retry_and_replay();
        schedule_and_deleted_camera();
        scheduled_boundaries_and_recovery();
        scheduled_peak_and_overnight();
    } catch (const std::exception& e) {
        fprintf(stderr, "task_runtime: %s\n", e.what());
        return 1;
    }
    if (!g_fail)
        printf("task_runtime PASSED\n");
    return g_fail;
}
