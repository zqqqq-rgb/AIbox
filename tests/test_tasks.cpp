#include "config_compiler.h"
#include "task_service.h"
#include "test_util.h"
#include <ctime>
#include <future>
#include <stdexcept>
#include <unistd.h>
using namespace aibox;
using namespace aibox::biz;
namespace {
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (0)
int64_t camera(Db& db, bool enabled = true) {
    CameraRow c;
    c.name = "task camera";
    c.rtsp_main = "rtsp://main";
    c.rtsp_sub = "rtsp://sub";
    c.enabled = enabled;
    const auto id = db.insert_camera(c);
    REQUIRE(id > 0);
    return id;
}
Json::Value draft(int64_t cam, std::string name = "task", double threshold = .6, int fps = 3) {
    Json::Value body;
    body["name"] = name;
    body["enabled"] = false;
    body["camera_ids"] = Json::Value(Json::arrayValue);
    body["camera_ids"].append(static_cast<Json::Int64>(cam));
    body["algorithms"] = Json::Value(Json::arrayValue);
    Json::Value algo;
    algo["algo_id"] = "person";
    algo["threshold"] = threshold;
    algo["fps"] = fps;
    body["algorithms"].append(algo);
    Json::Value rule;
    rule["key"] = "entrance";
    rule["algo_id"] = "person";
    rule["capability_id"] = "region_in";
    rule["roi"] = Json::Value(Json::arrayValue);
    for (auto pair : {std::pair<double, double>{0, 0}, {1, 0}, {1, 1}}) {
        Json::Value p(Json::arrayValue);
        p.append(pair.first);
        p.append(pair.second);
        rule["roi"].append(p);
    }
    body["rules"] = Json::Value(Json::arrayValue);
    body["rules"].append(rule);
    return body;
}
void registry(Db& db) {
    proto::RegistryReadyEvt r;
    proto::RegistryAlgo m;
    m.algo_id = "person";
    m.trigger = "steady";
    m.npu_cost_ms = 100;
    m.npu_mem_kb = 1000;
    r.algos.push_back(m);
    REQUIRE(db.replace_algorithms(r));
}
int64_t count(Db& db, const std::string& table) {
    auto st = db.prepare("SELECT count(*) FROM " + table);
    REQUIRE(st && st->step());
    return st->i64(0);
}
void test_legacy_authority_and_ids() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db);
    REQUIRE(db.set_camera_algos(cam, {{"person", .7, 4, "[[0,0],[1,0],[1,1]]", true,
                                       R"({"rule_type":"dwell","dwell_sec":12})", 0, "#ff8800"}}));
    auto tasks = db.list_tasks();
    REQUIRE(tasks.size() == 1 && tasks[0].kind == "legacy");
    const auto id = tasks[0].id, rule_id = tasks[0].rules[0].id, version = tasks[0].revision;
    auto rows = db.list_camera_algos(cam);
    REQUIRE(rows.size() == 1 && rows[0].roi_color == "#ff8800");
    const auto compiled = compile_task_plan(tasks, cam, 0, false);
    REQUIRE(compiled.models.at("person")["rules"][0]["debounce_sec"].asUInt() ==
            5); // Legacy default is preserved.
    REQUIRE(db.set_camera_algos(cam, rows));
    AlgorithmTask current;
    REQUIRE(db.get_task(id, current));
    REQUIRE(current.revision == version);
    rows[0].rule_json = R"({"rule_type":"dwell","dwell_sec":17})";
    REQUIRE(db.set_camera_algos(cam, rows));
    REQUIRE(db.get_task(id, current));
    REQUIRE(current.id == id && current.rules[0].id == rule_id && current.rules[0].version == 2);
    REQUIRE(current.revision == version + 1 && current.rules[0].params_json == rows[0].rule_json);
    REQUIRE(!db.exec("UPDATE camera_algos SET threshold=.1;"));
    REQUIRE(db.list_camera_algos(cam)[0].threshold == .7);
    auto body = draft(cam);
    TaskService service(db);
    REQUIRE(service.save(id, body).code == 1003);
    REQUIRE(service.remove(id, Json::Value()).code != 0);
    REQUIRE(db.set_camera_algos(cam, {}));
    REQUIRE(db.get_task(id, current));
    REQUIRE(current.algorithms.empty() && current.rules.empty());
}
void test_v5_migration_preserves_data() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db, false);
    REQUIRE(db.set_camera_algos(
        cam, {{"missing", .9, 2, "invalid old ROI", false, "invalid old rule", 0, "#123456"}}));
    EventRow event;
    event.camera_id = cam;
    event.type = "history";
    REQUIRE(db.insert_event(event) > 0);
    REQUIRE(db.exec("UPDATE camera_config_state SET apply_status='legacy_inactive';"
                    "CREATE TABLE old_algos AS SELECT * FROM camera_algos; DROP VIEW camera_algos; "
                    "DROP TRIGGER camera_legacy_task_insert;"
                    "DROP TABLE task_rules; DROP TABLE task_algorithms; DROP TABLE task_schedules; "
                    "DROP TABLE task_cameras;"
                    "DROP TABLE task_deployments; DROP TABLE algorithm_tasks; DROP TABLE capabilities; DROP TABLE event_rule_sources; ALTER TABLE old_algos "
                    "RENAME TO camera_algos; UPDATE schema_meta SET version=5;"));
    REQUIRE(db.exec(
        "CREATE TABLE trigger_anchor(value INTEGER); CREATE TRIGGER camera_algos_readonly_insert "
        "AFTER INSERT ON trigger_anchor BEGIN SELECT 1; END;"));
    REQUIRE(db.migrate() ==
            -1); // Fail after old data was copied and its table dropped inside the migration.
    auto original = db.prepare("SELECT roi_polygon FROM camera_algos;");
    REQUIRE(original && original->step() && original->text(0) == "invalid old ROI");
    original.reset();
    auto partial = db.prepare("SELECT count(*) FROM sqlite_master WHERE name='algorithm_tasks';");
    REQUIRE(partial && partial->step() && partial->i64(0) == 0);
    partial.reset();
    REQUIRE(db.exec("DROP TRIGGER camera_algos_readonly_insert; DROP TABLE trigger_anchor;"));
    REQUIRE(db.migrate() == Db::kSchemaLatest && db.migrate() == Db::kSchemaLatest);
    auto rows = db.list_camera_algos(cam);
    REQUIRE(rows.size() == 1 && !rows[0].enabled && rows[0].roi_json == "invalid old ROI");
    REQUIRE(rows[0].rule_json == "invalid old rule" && rows[0].roi_color == "#123456");
    REQUIRE(db.list_events(cam, "", 0, 0, 10).size() == 1);
    CameraConfigState state;
    REQUIRE(db.get_camera_config_state(cam, state) && state.apply_status == "legacy_inactive");
    auto tasks = db.list_tasks();
    REQUIRE(tasks.size() == 1 && tasks[0].rules.size() == 1);
    REQUIRE(tasks[0].algorithms[0].enabled == false);
    proto::SetAlgos compiled;
    std::string error;
    REQUIRE(compile_algos(cam, rows, compiled, error) && compiled.algos.empty());
    const auto another = camera(db);
    REQUIRE(db.list_tasks().size() == 2 && another != cam);
}
void test_drafts_versions_atomicity() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db);
    TaskService service(db);
    auto body = draft(cam);
    auto result = service.save(0, body);
    REQUIRE(result.code == 0 && !result.data["applied"].asBool());
    REQUIRE(!result.data["rules"][0]["params"].isMember(
        "rule_type")); // Returned params remain valid input.
    const auto id = result.data["id"].asInt64(), rule_id = result.data["rules"][0]["id"].asInt64();
    REQUIRE(rule_id > 0 && db.list_camera_algos(cam).empty() &&
            db.pending_config_operations() == 0);
    body["revision"] = 1;
    body["name"] = "renamed";
    auto changed = service.save(id, body);
    REQUIRE(changed.code == 0 && changed.data["revision"] == 2);
    REQUIRE(changed.data["rules"][0]["id"].asInt64() == rule_id &&
            changed.data["rules"][0]["version"] == 1);
    REQUIRE(service.save(id, body).code == 1003);
    body["revision"] = 2;
    body["rules"][0]["params"]["debounce_sec"] = 15;
    changed = service.save(id, body);
    REQUIRE(changed.code == 0 && changed.data["rules"][0]["version"] == 2);
    REQUIRE(changed.data["rules"][0]["id"].asInt64() == rule_id);
    const auto before = count(db, "algorithm_tasks");
    body = draft(cam);
    body["enabled"] = true;
    REQUIRE(service.save(0, body).code == 1003 && count(db, "algorithm_tasks") == before);
    body["enabled"] = false;
    REQUIRE(db.exec("CREATE TRIGGER fail_task_rule BEFORE INSERT ON task_rules BEGIN SELECT "
                    "RAISE(ABORT,'fault'); END;"));
    REQUIRE(service.save(0, body).code == 5001 && count(db, "algorithm_tasks") == before);
    body["revision"] = 3;
    body["name"] = "must roll back";
    REQUIRE(service.save(id, body).code == 5001);
    AlgorithmTask unchanged;
    REQUIRE(db.get_task(id, unchanged));
    REQUIRE(unchanged.name == "renamed" && unchanged.revision == 3 &&
            unchanged.rules[0].id == rule_id && unchanged.rules[0].version == 2);
    REQUIRE(db.exec("DROP TRIGGER fail_task_rule;"));
    REQUIRE(count(db, "task_cameras") == 2); // One legacy and one draft association.
    Json::Value revision;
    revision["revision"] = 3;
    const auto deleted = service.remove(id, revision);
    REQUIRE(deleted.code == 0 && deleted.data["status"] == "deleted" &&
            deleted.data["revision"] == 4 && service.get(id).code == 1002);
    AlgorithmTask historical;
    REQUIRE(db.get_task(id, historical) && historical.deleted_at > 0 &&
            historical.rules[0].id == rule_id);
    REQUIRE(db.list_tasks().size() == 1);
}
void test_validation_and_conflicts() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db);
    TaskService service(db);
    auto body = draft(cam);
    body["rules"][0]["capability_id"] = "line_cross";
    REQUIRE(service.save(0, body).code == 1001);
    body = draft(cam);
    body["rules"][0]["roi"][2] = body["rules"][0]["roi"][1];
    REQUIRE(service.save(0, body).code == 1001);
    body = draft(cam);
    body["rules"][0]["params"]["classes"].append(0);
    REQUIRE(service.save(0, body).code == 1001);
    body = draft(cam);
    body["camera_ids"].append(static_cast<Json::Int64>(cam));
    REQUIRE(service.save(0, body).code == 1001);
    body = draft(999);
    REQUIRE(service.save(0, body).code == 1002);
    body = draft(cam);
    auto saved = service.save(0, body);
    REQUIRE(saved.code == 0);
    body["revision"] = 1;
    const auto id = saved.data["id"].asInt64();
    std::promise<void> gate;
    auto ready = gate.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        ready.wait();
        TaskService s(db);
        return s.save(id, body).code;
    });
    auto second = std::async(std::launch::async, [&] {
        ready.wait();
        TaskService s(db);
        return s.save(id, body).code;
    });
    gate.set_value();
    const auto a = first.get(), b = second.get();
    REQUIRE((a == 0 && b == 1003) || (a == 1003 && b == 0));
}
void test_shared_preview_and_schedule() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db), cam2 = camera(db);
    registry(db);
    TaskService service(db);
    auto a = draft(cam, "first", .6, 3);
    a["camera_ids"].append(static_cast<Json::Int64>(cam2));
    auto b = draft(cam, "second", .4, 7);
    b["rules"][0]["key"] = "dwell-zone";
    b["rules"][0]["capability_id"] = "dwell";
    b["rules"][0]["params"]["dwell_sec"] = 12;
    b["rules"][0]["roi"][0][0] = .2;
    const auto first = service.save(0, a), second = service.save(0, b);
    REQUIRE(first.code == 0 && second.code == 0);
    Json::Value request;
    request["camera_id"] = static_cast<Json::Int64>(cam);
    request["at_ms"] = 0;
    auto hidden = service.preview(request);
    REQUIRE(hidden.code == 0 && hidden.data["models"].empty());
    request["include_drafts"] = true;
    auto preview = service.preview(request);
    REQUIRE(preview.code == 0 && preview.data["valid"].asBool());
    REQUIRE(preview.data["wire_valid"].asBool() && preview.data["required_rule_config_version"] == 2);
    REQUIRE(!preview.data["engine_capabilities_checked"].asBool());
    const auto models = preview.data["models"];
    REQUIRE(models.size() == 1 && models[0]["fps"] == 7 && models[0]["threshold"].asDouble() == .4);
    REQUIRE(models[0]["sources"].size() == 2 && models[0]["rules"].size() == 2);
    REQUIRE(models[0]["rules"][0]["task_id"] != models[0]["rules"][1]["task_id"]);
    REQUIRE(models[0]["rules"][0]["rule_id"] != models[0]["rules"][1]["rule_id"]);
    REQUIRE(models[0]["rules"][0]["threshold"].asDouble() == .6 &&
            models[0]["rules"][1]["threshold"].asDouble() == .4);
    REQUIRE(models[0]["rules"][0]["debounce_sec"].asUInt() == 30 &&
            models[0]["rules"][1]["dwell_sec"].asUInt() == 12);
    REQUIRE(models[0]["rules"][0]["roi"] != models[0]["rules"][1]["roi"]);
    CHECK_NEAR(preview.data["budget"]["compute_pct"].asDouble(), 62.5, .001);
    REQUIRE(!preview.data["apply_supported"].asBool() && !preview.data["applied"].asBool());
    REQUIRE(db.list_camera_algos(cam).empty());
    auto capabilities = service.capabilities();
    REQUIRE(capabilities.size() >= 10);
    for (const auto& cap : capabilities)
        REQUIRE(!cap["runtime_supported"].asBool());
}
int64_t monday(int hour = 0, int minute = 0) {
    std::tm value{};
    value.tm_year = 126;
    value.tm_mon = 9;
    value.tm_mday = 26;
    value.tm_hour = hour;
    value.tm_min = minute;
    return static_cast<int64_t>(timegm(&value)) * 1000;
}
void test_time_windows() {
    TaskSchedule night{"UTC+08:00", 1, 22 * 60, 2 * 60};
    REQUIRE(schedule_matches(night, monday(14)));
    REQUIRE(!schedule_matches(night, monday(13, 59)));
    REQUIRE(schedule_matches(night, monday(17, 59)));
    REQUIRE(!schedule_matches(night, monday(18)));
    TaskSchedule sunday{"UTC", 64, 23 * 60, 60};
    REQUIRE(schedule_matches(sunday, monday(0, 59)) && !schedule_matches(sunday, monday(1)));
    REQUIRE(!schedule_matches(sunday, monday(23)));
    REQUIRE(!valid_task_schedule({"Asia/Shanghai", 127, 0, 1440}));
    REQUIRE(!valid_task_schedule({"UTC+14:01", 127, 0, 1440}));
    REQUIRE(!valid_task_schedule({"UTC", 0, 0, 1440}));
    REQUIRE(!valid_task_schedule({"UTC", 127, 0, 0}));
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db);
    registry(db);
    TaskService service(db);
    auto body = draft(cam);
    Json::Value schedule;
    schedule["timezone"] = "UTC+08:00";
    schedule["weekdays"] = 1;
    schedule["start_minute"] = 1320;
    schedule["end_minute"] = 120;
    body["schedules"].append(schedule);
    REQUIRE(service.save(0, body).code == 0);
    Json::Value request;
    request["camera_id"] = static_cast<Json::Int64>(cam);
    request["include_drafts"] = true;
    request["at_ms"] = static_cast<Json::Int64>(monday(14));
    REQUIRE(service.preview(request).data["models"].size() == 1);
    request["at_ms"] = static_cast<Json::Int64>(monday(18));
    REQUIRE(service.preview(request).data["models"].empty());
}
void test_reopen_draft() {
    const auto path = "/tmp/aibox_task_" + std::to_string(::getpid()) + ".db";
    ::unlink(path.c_str());
    int64_t id = 0, rule_id = 0;
    {
        Db db(path);
        REQUIRE(db.migrate() == Db::kSchemaLatest);
        TaskService service(db);
        const auto result = service.save(0, draft(camera(db)));
        REQUIRE(result.code == 0);
        id = result.data["id"].asInt64();
        rule_id = result.data["rules"][0]["id"].asInt64();
    }
    {
        Db db(path);
        REQUIRE(db.migrate() == Db::kSchemaLatest);
        AlgorithmTask task;
        REQUIRE(db.get_task(id, task));
        REQUIRE(!task.enabled && task.rules[0].id == rule_id && task.revision == 1);
    }
    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());
}
void test_corrupt_stored_configuration_fails_closed() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    const auto cam = camera(db);
    TaskService service(db);
    const auto saved = service.save(0, draft(cam));
    REQUIRE(saved.code == 0);
    Json::Value request;
    request["camera_id"] = static_cast<Json::Int64>(cam);
    request["include_drafts"] = true;
    REQUIRE(db.exec("UPDATE task_rules SET params_json='{\"debounce_sec\":-1}';"));
    auto plan = service.preview(request);
    REQUIRE(plan.code == 0 && !plan.data["valid"].asBool() && !plan.data["errors"].empty());
    REQUIRE(db.exec("UPDATE task_rules SET params_json='{}'; UPDATE task_algorithms SET fps=99;"));
    plan = service.preview(request);
    REQUIRE(!plan.data["valid"].asBool() && plan.data["models"].empty());
    REQUIRE(db.exec("UPDATE task_algorithms SET fps=3; INSERT INTO task_schedules "
                    "VALUES(2,0,'Invalid/Zone',1,0,1440);"));
    plan = service.preview(request);
    REQUIRE(!plan.data["valid"].asBool() && plan.data["models"].empty());
}
} // namespace
int main() {
    for (auto test : {test_legacy_authority_and_ids, test_v5_migration_preserves_data,
                      test_drafts_versions_atomicity, test_validation_and_conflicts,
                      test_shared_preview_and_schedule, test_time_windows, test_reopen_draft,
                      test_corrupt_stored_configuration_fails_closed}) {
        try {
            test();
        } catch (const std::exception& error) {
            fprintf(stderr, "tasks failure: %s\n", error.what());
            g_fail = 1;
        }
    }
    return g_fail;
}
