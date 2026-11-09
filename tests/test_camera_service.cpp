#include "camera_service.h"
#include "fake_engine.h"
#include "http_api.h"
#include "mock_zlm.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <unistd.h>

using namespace aibox;
using namespace aibox::biz;
using aibox::testutil::FakeEngine;
using aibox::testutil::MockZlm;
namespace {
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            CHECK(x);                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)
template <class F> bool until(F predicate, int timeout = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (std::chrono::steady_clock::now() < end) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}
int sequence = 0;
struct Gate {
    std::promise<void> reached, release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> armed{true}, opened{false};
    void block() {
        if (armed.exchange(false)) {
            reached.set_value();
            released.wait();
        }
    }
    void open() {
        if (!opened.exchange(true))
            release.set_value();
    }
    ~Gate() {
        open();
    }
};
struct ScopedGate {
    std::shared_ptr<Gate> value = std::make_shared<Gate>();
    ~ScopedGate() {
        value->open();
    }
};
struct Rig {
    Db db;
    MockZlm media;
    FakeEngine engine;
    std::unique_ptr<ZlmClient> zlm;
    std::unique_ptr<UdsClient> uds;
    std::unique_ptr<CameraService> service;
    std::function<void()> replay_gate;
    explicit Rig(int wait_ms = 500, const std::string& path = ":memory:", bool connect = true,
                 int retry_ms = 30, int media_check_ms = 5000)
        : db(path) {
        CHECK(db.migrate() == Db::kSchemaLatest);
        proto::RegistryReadyEvt registry;
        proto::RegistryAlgo model;
        model.algo_id = "person";
        model.trigger = "steady";
        model.npu_cost_ms = 100;
        model.npu_mem_kb = 1000;
        registry.algos.push_back(model);
        model.algo_id = "face";
        registry.algos.push_back(model);
        CHECK(db.replace_algorithms(registry));
        CHECK(media.start());
        zlm =
            std::make_unique<ZlmClient>(ZlmClient::Params{"127.0.0.1", media.port(), "test", 300});
        const auto socket = "/tmp/aibox_service_" + std::to_string(::getpid()) + "_" +
                            std::to_string(++sequence) + ".sock";
        CHECK(engine.start(socket));
        UdsClient::Hooks hooks;
        hooks.prepare_camera = [this](const CameraConfigSnapshot& target) {
            if (replay_gate)
                replay_gate();
            auto reserved = db.reserved_config_snapshots(target.camera.id);
            std::string previous;
            for (size_t i = 1; i < reserved.size(); i += 2)
                if (!reserved[i].empty()) {
                    previous = reserved[i];
                    break;
                }
            return reconcile_camera_media(db, *zlm, target, previous);
        };
        UdsClient::Params client;
        client.connect_retry_ms = 10;
        client.ack_timeout_ms = 200;
        client.ack_retries = 0;
        client.ping_interval_ms = 10000;
        client.media_rtsp_port = 8554;
        uds = std::make_unique<UdsClient>(socket, db, hooks, client);
        if (connect) {
            uds->start();
            CHECK(until([&] { return uds->online(); }));
        }
        CameraService::Params params;
        params.apply_wait_ms = wait_ms;
        params.reconciler.media_port = 8554;
        params.reconciler.retry_ms = retry_ms;
        params.reconciler.poll_ms = 2;
        params.reconciler.media_check_ms = media_check_ms;
        service = std::make_unique<CameraService>(db, *uds, *zlm, params);
        CHECK(service->start());
    }
    ~Rig() {
        service->stop();
        uds->stop();
        engine.stop();
        media.stop();
    }
    bool status(const std::string& id, const char* expected) {
        ConfigOperationRow operation;
        return db.get_config_operation(id, operation) && operation.status == expected;
    }
};
CameraRow camera() {
    CameraRow value;
    value.name = "front";
    value.rtsp_main = "rtsp://camera/main";
    value.rtsp_sub = "rtsp://camera/sub";
    return value;
}
CamAlgoRow person(int fps = 3) {
    CamAlgoRow value;
    value.algo_id = "person";
    value.detect_fps = fps;
    value.roi_json = "[[0,0],[1,0],[1,1]]";
    value.rule_json = R"({"rule_type":"dwell","dwell_sec":12})";
    return value;
}
void test_lifecycle() {
    Rig r;
    auto created = r.service->create(camera());
    REQUIRE(created.code == 0 && created.accepted);
    const auto id = created.camera_id;
    proto::AddCam runtime;
    proto::SetAlgos applied;
    REQUIRE(r.engine.camera(id, runtime, applied));
    CHECK(runtime.rtsp_sub == "rtsp://127.0.0.1:8554/live/cam" + std::to_string(id) + "_sub");
    CHECK(r.media.source("cam" + std::to_string(id) + "_main") == camera().rtsp_main);
    CamAlgoRow disabled;
    disabled.algo_id = "uninstalled";
    disabled.enabled = false;
    REQUIRE(r.service->set_algos(id, {person(), disabled}).code == 0);
    REQUIRE(r.engine.camera(id, runtime, applied) && applied.algos.size() == 1);
    CHECK(applied.algos[0].dwell_sec == 12 && applied.algos[0].has_roi);
    const int adds = r.engine.add_cam, sets = r.engine.set_algos, deletes = r.media.del;
    CameraPatch rename;
    rename.name = "renamed";
    REQUIRE(r.service->update(id, rename).code == 0);
    CHECK(r.engine.add_cam == adds && r.engine.set_algos == sets && r.media.del == deletes);
    REQUIRE(r.service->set_algos(id, {}).code == 0);
    REQUIRE(r.engine.camera(id, runtime, applied));
    CHECK(applied.algos.empty());
    CHECK(r.engine.remove_cam == 0 && r.media.del == deletes);
    REQUIRE(r.service->set_algos(id, {person()}).code == 0);
    CameraPatch disable;
    disable.enabled = false;
    REQUIRE(r.service->update(id, disable).code == 0);
    CHECK(!r.engine.camera(id, runtime, applied));
    CHECK(r.db.list_cameras(false).size() == 1 && r.db.list_cameras(true).empty());
    CHECK(r.db.list_camera_algos(id).size() == 1);
    CHECK(r.media.source("cam" + std::to_string(id) + "_main").empty());
    CameraPatch enable;
    enable.enabled = true;
    REQUIRE(r.service->update(id, enable).code == 0);
    REQUIRE(r.engine.camera(id, runtime, applied));
    CHECK(applied.algos.size() == 1);
    EventRow event;
    event.camera_id = id;
    event.type = "retained";
    CHECK(r.db.insert_event(event) > 0);
    SegmentRow segment;
    segment.camera_id = id;
    segment.file_path = "/tmp/history.mp4";
    CHECK(r.db.insert_segment(segment) > 0);
    REQUIRE(r.service->remove(id).code == 0);
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(id, state));
    CHECK(state.deleted_at > 0 && state.desired_revision == state.applied_revision);
    CHECK(r.db.list_cameras(false).empty() && r.db.list_cameras(false, true).size() == 1);
    CHECK(r.db.list_events(id, "", 0, 0, 10).size() == 1 &&
          r.db.list_segments(id, 0, 0, 10).size() == 1);
    CHECK(r.service->update(id, enable).code == 1002);
}
void test_retry_and_restart() {
    const auto path = "/tmp/aibox_service_db_" + std::to_string(::getpid()) + ".db";
    ::unlink(path.c_str());
    std::string id;
    {
        Rig r(0, path);
        r.engine.fail_algorithms(1, true);
        auto result = r.service->create(camera());
        REQUIRE(result.accepted && result.code == 1003);
        id = result.operation_id;
        REQUIRE(until([&] { return r.status(id, "retry_wait"); }));
        CameraConfigState state;
        REQUIRE(r.db.get_camera_config_state(1, state));
        CHECK(state.desired_revision == 1 && state.applied_revision == 0);
    }
    {
        Rig r(500, path); // Fresh engine/media, reopened DB; replay resumes the same operation.
        REQUIRE(until([&] { return r.status(id, "succeeded"); }));
        CameraConfigState state;
        REQUIRE(r.db.get_camera_config_state(1, state));
        CHECK(state.desired_revision == state.applied_revision && state.applied_revision == 1);
        proto::AddCam runtime;
        proto::SetAlgos algos;
        CHECK(r.engine.camera(1, runtime, algos));
    }
    ::unlink(path.c_str());
}
void test_executed_without_ack_replays_after_disconnect() {
    Rig r;
    const auto created = r.service->create(camera());
    REQUIRE(created.code == 0);
    ScopedGate scope;
    auto gate = scope.value;
    auto reached = gate->reached.get_future();
    r.engine.after_set_algos([gate] { gate->block(); });
    auto pending = std::async(std::launch::async,
                              [&] { return r.service->set_algos(created.camera_id, {person(7)}); });
    REQUIRE(reached.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    proto::AddCam runtime;
    proto::SetAlgos applied;
    REQUIRE(r.engine.camera(created.camera_id, runtime, applied));
    CHECK(applied.algos.size() == 1 && applied.algos[0].fps == 7);
    const auto old_epoch = r.uds->connection_epoch();
    r.uds->stop(); // Closes transport before the execution ACK can be delivered.
    const auto result = pending.get();
    CHECK(result.accepted && result.code == 1003);
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
    CHECK(state.applied_revision < state.desired_revision);
    CHECK(r.uds->connection_epoch() != old_epoch);
    gate->open();
    r.uds->start();
    REQUIRE(until([&] { return r.uds->online() && r.status(result.operation_id, "succeeded"); }));
    REQUIRE(r.engine.camera(created.camera_id, runtime, applied));
    CHECK(applied.algos.size() == 1 && applied.algos[0].fps == 7);
}
void test_syncing_rejects_without_side_effects() {
    Rig r(500, ":memory:", false);
    auto seed = camera();
    const auto id = r.db.insert_camera(seed);
    REQUIRE(id > 0);
    ScopedGate scope;
    auto gate = scope.value;
    auto reached = gate->reached.get_future();
    r.replay_gate = [gate] { gate->block(); };
    r.uds->start();
    REQUIRE(reached.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(r.uds->state() == UdsClient::State::SYNCING);
    auto result = r.service->create(camera());
    CHECK(result.code == 1003 && !result.accepted);
    CameraPatch patch;
    patch.name = "must-not-save";
    CHECK(!r.service->update(id, patch).accepted);
    CameraRow current;
    REQUIRE(r.db.get_camera(id, current));
    CHECK(current.name == "front");
    CHECK(r.db.list_cameras(false).size() == 1 && r.db.pending_config_operations() == 0);
    CHECK(r.media.add_main == 0 && r.media.add_sub == 0);
    gate->open();
    REQUIRE(until([&] { return r.uds->online(); }));
}
void test_media_compensation_and_recovery() {
    Rig r(0, ":memory:", true, 500);
    auto created = r.service->create(camera());
    REQUIRE(created.accepted);
    REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
    r.media.reject_source("rtsp://new/sub", true);
    CameraPatch patch;
    patch.rtsp_main = "rtsp://new/main";
    patch.rtsp_sub = "rtsp://new/sub";
    auto changed = r.service->update(created.camera_id, patch);
    REQUIRE(changed.accepted);
    REQUIRE(until([&] { return r.status(changed.operation_id, "retry_wait"); }));
    CHECK(r.media.source("cam1_main") == camera().rtsp_main);
    CHECK(r.media.source("cam1_sub") == camera().rtsp_sub);
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(1, state));
    CHECK(state.desired_revision > state.applied_revision);
    r.media.reject_source("rtsp://new/sub", false);
    REQUIRE(until([&] { return r.status(changed.operation_id, "succeeded"); }));
    CHECK(r.media.source("cam1_main") == "rtsp://new/main" &&
          r.media.source("cam1_sub") == "rtsp://new/sub");
}
void test_budget_reservation_until_confirmed_stop() {
    Rig r(0, ":memory:", true, 500);
    auto a = r.service->create(camera());
    REQUIRE(a.accepted);
    REQUIRE(until([&] { return r.status(a.operation_id, "succeeded"); }));
    auto b = r.service->create(camera());
    REQUIRE(b.accepted);
    REQUIRE(until([&] { return r.status(b.operation_id, "succeeded"); }));
    auto active = r.service->set_algos(a.camera_id, {person(10)});
    REQUIRE(active.accepted);
    REQUIRE(until([&] { return r.status(active.operation_id, "succeeded"); }));
    r.engine.fail_algorithms(a.camera_id, true);
    auto stopped = r.service->set_algos(a.camera_id, {});
    REQUIRE(stopped.accepted);
    REQUIRE(until([&] { return r.status(stopped.operation_id, "retry_wait"); }));
    const auto rejected = r.service->set_algos(b.camera_id, {person(10)});
    CHECK(rejected.code == 3001 && !rejected.accepted);
    CHECK(r.db.list_camera_algos(b.camera_id).empty());
    r.engine.fail_algorithms(a.camera_id, false);
    REQUIRE(until([&] { return r.status(stopped.operation_id, "succeeded"); }));
    auto accepted = r.service->set_algos(b.camera_id, {person(10)});
    CHECK(accepted.accepted);
}
void test_partitioned_budget() {
    PrecheckInput input;
    input.models = {{"trigger", 100, 100, true}};
    input.usages = {{"trigger", 1, 9}};
    input.npu_cores = 3;
    CHECK(npu_precheck(input).code ==
          3001); // One triggered worker: extra steady cores cannot hide overload.
    input.usages[0].fps = 8;
    CHECK(npu_precheck(input).ok);
    input.usages = {{"trigger", 1, 4}, {"trigger", 2, 4}};
    input.npu_mem_total_kb = 125;
    CHECK(npu_precheck(input).ok); // Shared physical model uses memory once.
    input.models[0].cost_ms = 0;
    CHECK(npu_precheck(input).code == 1003);
}
void test_reported_core_plan_limits_admission() {
    Rig r;
    auto created = r.service->create(camera());
    REQUIRE(created.code == 0);
    r.engine.metrics(R"({"npu":{"steady_cores":[0],"triggered_cores":[1]}})");
    const auto result = r.service->set_algos(created.camera_id, {person(9)});
    CHECK(result.code == 3001 && !result.accepted); // 900 ms exceeds the actual one-worker budget.
    CHECK(r.db.list_camera_algos(created.camera_id).empty());
}
void test_media_restart_without_engine_disconnect() {
    Rig r(500, ":memory:", true, 30, 30);
    const auto created = r.service->create(camera());
    REQUIRE(created.code == 0);
    const int engine_adds = r.engine.add_cam;
    r.media.reset_proxies();
    REQUIRE(until([&] {
        return r.media.source("cam1_main") == camera().rtsp_main &&
               r.media.source("cam1_sub") == camera().rtsp_sub;
    }));
    CHECK(r.engine.add_cam == engine_adds);
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
    CHECK(state.applied_revision == 1 && state.desired_revision == 1);
}
void test_concurrent_admission_does_not_overbook() {
    Rig r(0);
    auto a = r.service->create(camera());
    REQUIRE(a.accepted);
    REQUIRE(until([&] { return r.status(a.operation_id, "succeeded"); }));
    auto b = r.service->create(camera());
    REQUIRE(b.accepted);
    REQUIRE(until([&] { return r.status(b.operation_id, "succeeded"); }));
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto left = std::async(std::launch::async, [&] {
        ready.wait();
        return r.service->set_algos(a.camera_id, {person(10)});
    });
    auto right = std::async(std::launch::async, [&] {
        ready.wait();
        return r.service->set_algos(b.camera_id, {person(10)});
    });
    start.set_value();
    const auto first = left.get(), second = right.get();
    CHECK(first.accepted != second.accepted);
    const auto rejected = first.accepted ? second : first;
    CHECK(rejected.code == 1003 || rejected.code == 3001);
}
void test_revert_after_uncertain_application_is_not_a_name_only_noop() {
    Rig r(0, ":memory:", true, 500);
    auto created = r.service->create(camera());
    REQUIRE(created.accepted);
    REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
    r.engine.fail_algorithms(1, true);
    CameraPatch change;
    change.rtsp_main = "rtsp://new/main";
    change.rtsp_sub = "rtsp://new/sub";
    auto failed = r.service->update(1, change);
    REQUIRE(failed.accepted);
    REQUIRE(until([&] { return r.status(failed.operation_id, "retry_wait"); }));
    CHECK(r.media.source("cam1_main") == "rtsp://new/main");
    r.engine.fail_algorithms(1, false);
    CameraPatch revert;
    revert.name = "original source restored";
    revert.rtsp_main = camera().rtsp_main;
    revert.rtsp_sub = camera().rtsp_sub;
    auto latest = r.service->update(1, revert);
    REQUIRE(latest.accepted);
    REQUIRE(until([&] { return r.status(latest.operation_id, "succeeded"); }));
    CHECK(r.status(failed.operation_id, "superseded"));
    CHECK(r.media.source("cam1_main") == camera().rtsp_main &&
          r.media.source("cam1_sub") == camera().rtsp_sub);
}
void test_malformed_media_response_remains_recoverable() {
    Rig r(0, ":memory:", true, 500);
    r.media.invalid_response = true;
    const auto created = r.service->create(camera());
    REQUIRE(created.accepted);
    REQUIRE(until([&] { return r.status(created.operation_id, "retry_wait"); }));
    r.media.invalid_response = false;
    REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
}
void test_failed_startup_replay_recovers_without_another_reconnect() {
    Rig r(500, ":memory:", true, 100);
    auto created = r.service->create(camera());
    REQUIRE(created.code == 0);
    auto configured = r.service->set_algos(created.camera_id, {person()});
    REQUIRE(configured.code == 0);
    r.uds->stop();
    r.engine.reset_state();
    r.media.reset_proxies();
    r.media.invalid_response = true;
    r.uds->start();
    REQUIRE(until([&] { return r.uds->online(); }));
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
    CHECK(state.apply_status != "succeeded");
    r.media.invalid_response = false;
    REQUIRE(until([&] { return r.status(configured.operation_id, "succeeded"); }));
    proto::AddCam runtime;
    proto::SetAlgos algos;
    REQUIRE(r.engine.camera(created.camera_id, runtime, algos));
    CHECK(algos.algos.size() == 1 && algos.algos[0].dwell_sec == 12);
}
void test_legacy_row_gets_a_durable_replay_operation() {
    Rig r(500, ":memory:", false, 100);
    auto old = camera();
    const auto id = r.db.insert_camera(old);
    REQUIRE(id > 0);
    CHECK(r.db.set_camera_algos(id, {person()}));
    r.media.invalid_response = true;
    r.uds->start();
    REQUIRE(until([&] { return r.uds->online(); }));
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(id, state));
    CHECK(state.desired_revision == 1 && state.applied_revision == 0);
    CHECK(r.db.pending_config_operations() == 1);
    r.media.invalid_response = false;
    REQUIRE(until([&] {
        return r.db.get_camera_config_state(id, state) && state.apply_status == "succeeded";
    }));
    proto::AddCam runtime;
    proto::SetAlgos algos;
    REQUIRE(r.engine.camera(id, runtime, algos));
    CHECK(algos.algos.size() == 1);
}
void test_confirmation_failure_is_not_success() {
    Rig r(20, ":memory:", true, 30);
    REQUIRE(r.db.exec("CREATE TRIGGER fail_confirmation BEFORE UPDATE OF applied_revision "
                      "ON camera_config_state WHEN NEW.applied_revision > OLD.applied_revision "
                      "BEGIN SELECT RAISE(FAIL, 'injected confirmation failure'); END;"));
    const auto result = r.service->create(camera());
    REQUIRE(result.accepted && result.code == 1003);
    proto::AddCam runtime;
    proto::SetAlgos algos;
    REQUIRE(until([&] { return r.engine.camera(result.camera_id, runtime, algos); }));
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(result.camera_id, state));
    CHECK(state.applied_revision == 0 && state.desired_revision == 1);
    CHECK(!r.status(result.operation_id, "succeeded"));
    REQUIRE(r.db.exec("DROP TRIGGER fail_confirmation;"));
    REQUIRE(until([&] { return r.status(result.operation_id, "succeeded"); }));
    REQUIRE(r.db.get_camera_config_state(result.camera_id, state));
    CHECK(state.applied_revision == 1);
}
void test_compensation_failure_remains_visible_and_recovers() {
    Rig r(0, ":memory:", true, 1000);
    auto created = r.service->create(camera());
    REQUIRE(created.accepted);
    REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
    r.media.reject_source("rtsp://new/sub", true);
    r.media.reject_source(camera().rtsp_main, true); // Both the change and its compensation fail.
    CameraPatch patch;
    patch.rtsp_main = "rtsp://new/main"; patch.rtsp_sub = "rtsp://new/sub";
    const auto changed = r.service->update(created.camera_id, patch);
    REQUIRE(changed.accepted && changed.code == 1003);
    REQUIRE(until([&] { return r.status(changed.operation_id, "retry_wait"); }));
    ConfigOperationRow operation;
    REQUIRE(r.db.get_config_operation(changed.operation_id, operation));
    CHECK(operation.last_error.find("compensation pending") != std::string::npos);
    CHECK(operation.last_error.find("rtsp://") == std::string::npos);
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
    CHECK(state.applied_revision < state.desired_revision);
    CHECK(r.media.source("cam1_main").empty());
    CHECK(r.media.source("cam1_sub") == camera().rtsp_sub);
    r.media.reject_source("rtsp://new/sub", false);
    r.media.reject_source(camera().rtsp_main, false);
    REQUIRE(until([&] { return r.status(changed.operation_id, "succeeded"); }));
    REQUIRE(r.db.get_config_operation(changed.operation_id, operation));
    CHECK(operation.last_error.empty());
    CHECK(r.media.source("cam1_main") == "rtsp://new/main");
    CHECK(r.media.source("cam1_sub") == "rtsp://new/sub");
}
void test_add_executed_but_ack_lost_is_rebuilt() {
    Rig r(500, ":memory:", true, 20);
    ScopedGate scope;
    auto gate = scope.value;
    auto reached = gate->reached.get_future();
    auto* engine = &r.engine;
    r.engine.command_boundary([gate, engine](const std::string& command, bool before) {
        if (command == "add_cam" && !before && gate->armed.load()) {
            engine->disconnect_clients();
            gate->block();
        }
    });
    const auto result = r.service->create(camera());
    REQUIRE(result.accepted && result.code == 1003);
    REQUIRE(reached.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    proto::AddCam runtime; proto::SetAlgos algos;
    REQUIRE(r.engine.camera(result.camera_id, runtime, algos));
    CameraConfigState state;
    REQUIRE(r.db.get_camera_config_state(result.camera_id, state));
    CHECK(state.applied_revision < state.desired_revision);
    const auto media_deletes = r.media.del.load();
    gate->open();
    REQUIRE(until([&] { return r.uds->online() && r.status(result.operation_id, "succeeded"); }));
    REQUIRE(r.engine.camera(result.camera_id, runtime, algos));
    CHECK(r.engine.remove_cam >= 1 && r.engine.add_cam >= 3 && r.engine.set_algos >= 1);
    CHECK(r.media.del == media_deletes); // Recovery does not tear down the newly prepared media.
    r.engine.command_boundary({});
}
void test_disconnect_while_old_lease_is_still_unwinding() {
    Rig r;
    const auto created = r.service->create(camera());
    REQUIRE(created.code == 0);
    auto lease = r.uds->begin_configuration();
    REQUIRE(lease);
    const auto epoch = lease->epoch();
    ScopedGate scope;
    auto gate = scope.value;
    auto reached = gate->reached.get_future();
    r.engine.command_boundary([gate](const std::string& command, bool before) {
        if (command == "set_algos" && !before) gate->block();
    });
    proto::SetAlgos target;
    std::string error;
    REQUIRE(compile_algos(created.camera_id, {person(6)}, target, error));
    auto old_call = std::async(std::launch::async, [&] { return r.uds->set_algos(target); });
    REQUIRE(reached.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    r.engine.disconnect_clients();
    REQUIRE(until([&] { return r.uds->connection_epoch() != epoch; }));
    REQUIRE(old_call.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    CHECK(old_call.get().ret != 0);
    CHECK(!lease->valid() && !r.uds->begin_configuration());
    const auto commands = r.engine.commands();
    CHECK(r.uds->set_algos(target).ret != 0); // Old worker cannot write into the next connection.
    CHECK(r.engine.commands() == commands);
    gate->open();
    // Handoff remains blocked until the previous worker releases its configuration lease.
    CHECK(r.uds->state() == UdsClient::State::DISCONNECTED);
    lease.reset();
    REQUIRE(until([&] { return r.uds->online(); })); // Automatic reconnect; never call stop/start.
    proto::AddCam runtime; proto::SetAlgos restored;
    REQUIRE(r.engine.camera(created.camera_id, runtime, restored));
    CHECK(restored.algos.empty()); // Durable desired state wins over the unconfirmed old command.
    r.engine.command_boundary({});
}
void test_command_errors_are_not_blanket_success() {
    {
        Rig r;
        r.engine.respond_once("add_cam", 1002);
        const auto result = r.service->create(camera());
        REQUIRE(result.accepted && result.code == 1002 && result.apply_status == "failed");
        CHECK(r.engine.remove_cam == 0 && r.engine.set_algos == 0);
    }
    // A duplicate-like create response triggers one conservative rebuild, including full algorithms.
    {
        Rig r;
        r.engine.respond_once("add_cam", 1003);
        const auto result = r.service->create(camera());
        REQUIRE(result.code == 0);
        CHECK(r.engine.add_cam == 2 && r.engine.remove_cam == 1 && r.engine.set_algos == 1);
    }
    // set_algos 1002 retries through add/remove/add, then an unavailable model is a terminal failure.
    {
        Rig r;
        const auto created = r.service->create(camera()); REQUIRE(created.code == 0);
        r.engine.respond_once("set_algos", 1002); r.engine.respond_once("set_algos", 1002);
        const auto changed = r.service->set_algos(created.camera_id, {person()});
        REQUIRE(changed.accepted && changed.code == 1002 && changed.apply_status == "failed");
        CameraConfigState state; REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
        CHECK(state.applied_revision < state.desired_revision);
    }
    // A remove 1003 is retried, never treated as 'already absent'.
    {
        Rig r(0, ":memory:", true, 1000);
        const auto created = r.service->create(camera()); REQUIRE(created.accepted);
        REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
        r.engine.respond_once("remove_cam", 1003);
        CameraPatch stopped; stopped.enabled = false;
        const auto result = r.service->update(created.camera_id, stopped); REQUIRE(result.accepted);
        REQUIRE(until([&] { return r.status(result.operation_id, "retry_wait"); }));
        proto::AddCam camera; proto::SetAlgos algos;
        CHECK(r.engine.camera(created.camera_id, camera, algos));
        CHECK(!r.media.source("cam1_main").empty());
        REQUIRE(until([&] { return r.status(result.operation_id, "succeeded"); }));
        CHECK(!r.engine.camera(created.camera_id, camera, algos));
    }
    {
        Rig r(0, ":memory:", true, 1000);
        const auto created = r.service->create(camera()); REQUIRE(created.accepted);
        REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
        r.engine.respond_once("set_algos", 1003);
        const auto result = r.service->set_algos(created.camera_id, {person(4)});
        REQUIRE(result.accepted);
        REQUIRE(until([&] { return r.status(result.operation_id, "retry_wait"); }));
        CameraConfigState state; REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
        CHECK(state.applied_revision < state.desired_revision);
        REQUIRE(until([&] { return r.status(result.operation_id, "succeeded"); }));
        proto::AddCam camera; proto::SetAlgos algos;
        REQUIRE(r.engine.camera(created.camera_id, camera, algos));
        CHECK(algos.algos.size() == 1 && algos.algos[0].fps == 4);
    }
    {
        Rig r;
        const auto created = r.service->create(camera()); REQUIRE(created.code == 0);
        r.engine.reset_state(); // A genuine already-absent channel produces remove 1002.
        CameraPatch stopped; stopped.enabled = false;
        const auto result = r.service->update(created.camera_id, stopped);
        REQUIRE(result.code == 0);
        CHECK(r.media.source("cam1_main").empty() && r.media.source("cam1_sub").empty());
    }
}
void test_duplicate_pending_target_reuses_operation() {
    Rig r(0, ":memory:", true, 1000);
    const auto created = r.service->create(camera()); REQUIRE(created.accepted);
    REQUIRE(until([&] { return r.status(created.operation_id, "succeeded"); }));
    r.engine.fail_algorithms(created.camera_id, true);
    const auto first = r.service->set_algos(created.camera_id, {person()}); REQUIRE(first.accepted);
    REQUIRE(until([&] { return r.status(first.operation_id, "retry_wait") && r.service->write_ready(); }));
    const auto duplicate = r.service->set_algos(created.camera_id, {person()});
    REQUIRE(duplicate.accepted && duplicate.code == 1003);
    CHECK(first.operation_id == duplicate.operation_id && first.revision == duplicate.revision);
    CameraConfigState state; REQUIRE(r.db.get_camera_config_state(created.camera_id, state));
    CHECK(state.desired_revision == first.revision);
    r.engine.fail_algorithms(created.camera_id, false);
    REQUIRE(until([&] { return r.status(first.operation_id, "succeeded"); }));
    r.engine.respond_once("remove_cam", 1003);
    const auto removed = r.service->remove(created.camera_id); REQUIRE(removed.accepted);
    REQUIRE(until([&] { return r.status(removed.operation_id, "retry_wait") && r.service->write_ready(); }));
    const auto same_delete = r.service->remove(created.camera_id);
    REQUIRE(same_delete.accepted && same_delete.operation_id == removed.operation_id);
    CHECK(same_delete.revision == removed.revision);
    REQUIRE(until([&] { return r.status(removed.operation_id, "succeeded"); }));
}
void test_waiter_observes_superseded_as_terminal() {
    Rig r(5000, ":memory:", true, 1000);
    const auto created = r.service->create(camera()); REQUIRE(created.code == 0);
    r.engine.respond_once("set_algos", 1003);
    auto waiting = std::async(std::launch::async, [&] {
        return r.service->set_algos(created.camera_id, {person()});
    });
    ConfigOperationRow old;
    REQUIRE(until([&] { return r.db.get_current_config_operation(created.camera_id, old) &&
        old.status == "retry_wait" && r.service->write_ready(); }));
    const auto newer = r.service->set_algos(created.camera_id, {});
    REQUIRE(newer.code == 0);
    REQUIRE(waiting.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    const auto result = waiting.get();
    CHECK(result.accepted && result.code == 1003 && result.apply_status == "superseded");
    CHECK(result.operation_id == old.operation_id);
}
void run() {
    test_lifecycle();
    test_retry_and_restart();
    test_executed_without_ack_replays_after_disconnect();
    test_syncing_rejects_without_side_effects();
    test_media_compensation_and_recovery();
    test_budget_reservation_until_confirmed_stop();
    test_partitioned_budget();
    test_reported_core_plan_limits_admission();
    test_media_restart_without_engine_disconnect();
    test_concurrent_admission_does_not_overbook();
    test_revert_after_uncertain_application_is_not_a_name_only_noop();
    test_malformed_media_response_remains_recoverable();
    test_failed_startup_replay_recovers_without_another_reconnect();
    test_legacy_row_gets_a_durable_replay_operation();
    test_confirmation_failure_is_not_success();
    test_compensation_failure_remains_visible_and_recovers();
    test_add_executed_but_ack_lost_is_rebuilt();
    test_disconnect_while_old_lease_is_still_unwinding();
    test_command_errors_are_not_blanket_success();
    test_duplicate_pending_target_reuses_operation();
    test_waiter_observes_superseded_as_terminal();
}
} // namespace
TEST_MAIN("camera_service")
