#include "event_service.h"
#include "test_util.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace aibox::biz;
namespace {
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
Json::Value transition(const char* status, int64_t revision) {
    Json::Value body;
    body["disposition"] = status;
    body["revision"] = Json::Int64(revision);
    return body;
}

void test_disposition() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    EventRow event;
    event.camera_id = 3;
    event.algo_id = "person";
    event.type = "person_dwell";
    const auto id = db.insert_event(event);
    REQUIRE(id > 0);
    EventService service(db);
    CHECK(service.set_disposition(0, transition("ignored", 0)).code == 1001);
    CHECK(service.set_disposition(999, transition("ignored", 0)).code == 1002);
    CHECK(service.set_disposition(id, transition("invalid", 0)).code == 1001);
    CHECK(service.set_disposition(id, transition("pending", -1)).code == 1001);
    auto invalid = transition("ignored", 0);
    invalid["revision"] = 0.0;
    CHECK(service.set_disposition(id, invalid).code == 1001);

    auto result = service.set_disposition(id, transition("resolved", 0));
    CHECK(result.code == 1003 && result.data["reason_code"] == "transition_not_allowed");
    CHECK(result.data["disposition"] == "pending");
    CHECK(result.data["allowed_dispositions"].size() == 2);
    int64_t revision = 0;
    for (const auto* status : {"acknowledged", "processing", "resolved", "pending", "ignored", "pending"}) {
        result = service.set_disposition(id, transition(status, revision));
        REQUIRE(result.code == 0 && result.data["changed"].asBool());
        CHECK(result.data["disposition"] == status);
        CHECK(result.data["disposition_revision"].asInt64() == ++revision);
    }
    result = service.set_disposition(id, transition("ignored", revision - 1));
    CHECK(result.code == 1003 && result.data["reason_code"] == "revision_conflict");
    result = service.set_disposition(id, transition("pending", revision));
    CHECK(result.code == 0 && !result.data["changed"].asBool());

    REQUIRE(db.exec("CREATE TRIGGER reject_disposition_history BEFORE INSERT ON event_disposition_history "
                    "BEGIN SELECT RAISE(ABORT,'history write failure'); END;"));
    CHECK(service.set_disposition(id, transition("ignored", revision)).code == 5001);
    const auto detail = service.detail(id).data;
    CHECK(detail["disposition"] == "pending" && detail["disposition_revision"].asInt64() == revision);
    CHECK(detail["important_revision"].asInt64() == 0);
    auto history = db.prepare("SELECT COUNT(*),MIN(changed_at) FROM event_disposition_history;");
    REQUIRE(history && history->step());
    CHECK(history->i64(0) == revision && history->i64(1) > 0);
}

void test_disposition_records() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    EventRow event;
    event.camera_id = 3;
    const auto id = db.insert_event(event);
    REQUIRE(id > 0);
    EventService service(db);
    const auto empty = service.disposition_history(id);
    CHECK(empty.code == 0 && empty.data["entries"].isArray() && empty.data["entries"].empty());
    CHECK(service.disposition_history(0).code == 1001);
    CHECK(service.disposition_history(id, -1).code == 1001);
    CHECK(service.disposition_history(999).code == 1002);

    auto body = transition("acknowledged", 0);
    body["operator_name"] = "  值班员 A  ";
    body["note"] = "  人员仍在区域内\n已联系现场  ";
    auto bad = body;
    bad["operator_name"] = "line\nbreak";
    CHECK(service.set_disposition(id, bad).code == 1001);
    bad = body;
    bad["note"] = std::string(1001, 'x');
    CHECK(service.set_disposition(id, bad).code == 1001);
    bad = body;
    bad["note"] = std::string("one\0two", 7);
    CHECK(service.set_disposition(id, bad).code == 1001);
    bad = body;
    bad["changed_at"] = 1;
    CHECK(service.set_disposition(id, bad).code == 1001);
    REQUIRE(service.set_disposition(id, body).code == 0);
    body["revision"] = 1;
    CHECK(service.set_disposition(id, body).data["reason_code"] == "unchanged_disposition");
    body["revision"] = 0;
    CHECK(service.set_disposition(id, body).data["reason_code"] == "revision_conflict");
    const auto first = service.disposition_history(id).data["entries"];
    REQUIRE(first.size() == 1);
    CHECK(first[0]["from_status"] == "pending" && first[0]["to_status"] == "acknowledged");
    CHECK(first[0]["operator_name"] == "值班员 A" && first[0]["note"] == "人员仍在区域内\n已联系现场");
    CHECK(first[0]["changed_at"].asInt64() > 0);
    for (int64_t revision = 1; revision < 22; ++revision)
        REQUIRE(service.set_disposition(id, transition(revision % 2 ? "ignored" : "pending", revision)).code == 0);
    const auto newest = service.disposition_history(id).data;
    REQUIRE(newest["entries"].size() == 20 && newest["next_before_revision"].asInt64() == 3);
    CHECK(newest["entries"][0]["revision"].asInt64() == 22);
    CHECK(newest["entries"][0]["operator_name"].isNull() && newest["entries"][0]["note"].isNull());
    const auto older = service.disposition_history(id, 3).data;
    REQUIRE(older["entries"].size() == 2 && older["next_before_revision"].isNull());
    CHECK(older["entries"][0]["revision"].asInt64() == 2 && older["entries"][1] == first[0]);
}

void run() {
    Db db(":memory:"); REQUIRE(db.migrate() == Db::kSchemaLatest);
    EventService service(db);
    CHECK(service.detail(0).code == 1001);
    CHECK(service.detail(-1).code == 1001);
    CHECK(service.detail(999).code == 1002);
    EventRow event;
    event.camera_id = 123; event.algo_id = "person"; event.type = "REGION_IN";
    event.ts = 1000; event.targets_json = R"([{"cls":0,"score":0.9}])";
    event.task_id = 81; event.rule_id = 82; event.task_revision = 3; event.rule_version = 4;
    event.snapshot_path = "/data/recordings/snap/example.jpg";
    const auto id = db.insert_event(event); REQUIRE(id > 0);
    auto result = service.detail(id);
    REQUIRE(result.code == 0);
    CHECK(result.data["camera_id"].asInt64() == 123);
    CHECK(result.data["task_id"].asInt64() == 81 && result.data["rule_id"].asInt64() == 82);
    CHECK(result.data["task_revision"].asInt64() == 3 && result.data["rule_version"].asInt64() == 4);
    CHECK(result.data["snapshot"].asString() == event.snapshot_path);
    CHECK(result.data["targets_valid"].asBool() && result.data["targets"][0]["cls"].asInt() == 0);
    CHECK(!result.data["reported"].asBool());
    // These historical identifiers are intentionally not FKs to live tasks or cameras.
    CHECK(db.list_cameras(false).empty() && db.list_tasks().empty());
    CHECK(service.detail(id).data == result.data);

    event.task_id = event.rule_id = event.task_revision = event.rule_version = 0;
    event.targets_json = "broken historic metadata"; event.snapshot_path.clear(); event.reported = true;
    const auto legacy = db.insert_event(event); REQUIRE(legacy > 0);
    result = service.detail(legacy); REQUIRE(result.code == 0);
    CHECK(result.data["task_id"].asInt64() == 0 && result.data["rule_version"].asInt64() == 0);
    CHECK(!result.data["targets_valid"].asBool() && result.data["targets"].isNull());
    CHECK(!result.data.isMember("snapshot") && result.data["reported"].asBool());
    REQUIRE(db.exec("INSERT INTO events(id,camera_id,algo_id,type,ts) VALUES(9223372036854775806,1,'person','TEST',0);"));
    CHECK(service.detail(INT64_MAX - 1).data["id"].asInt64() == INT64_MAX - 1);
    Db unavailable(":memory:");
    CHECK(EventService(unavailable).detail(1).code == 5001);

    char file[] = "/tmp/aibox_event_detail_XXXXXX";
    const int fd = mkstemp(file); REQUIRE(fd >= 0); close(fd);
    int64_t persisted;
    {
        Db disk(file); REQUIRE(disk.migrate() == Db::kSchemaLatest);
        persisted = disk.insert_event(event); REQUIRE(persisted > 0);
        REQUIRE(disk.exec("DROP TABLE event_disposition_history;"
                          "ALTER TABLE events DROP COLUMN disposition; ALTER TABLE events DROP COLUMN disposition_revision;"
                          "UPDATE schema_meta SET version=9;"));
    }
    {
        Db reopened(file); REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        const auto migrated = EventService(reopened).detail(persisted).data;
        CHECK(migrated["type"] == "REGION_IN" && migrated["reported"].asBool());
        CHECK(migrated["disposition"] == "pending" && migrated["disposition_revision"].asInt64() == 0);
        Db second(file);
        std::atomic<bool> start{false};
        int first_code = -1, second_code = -1;
        std::thread first([&] {
            while (!start.load()) std::this_thread::yield();
            first_code = EventService(reopened).set_disposition(persisted, transition("acknowledged", 0)).code;
        });
        std::thread other([&] {
            while (!start.load()) std::this_thread::yield();
            second_code = EventService(second).set_disposition(persisted, transition("ignored", 0)).code;
        });
        start = true;
        first.join();
        other.join();
        CHECK((first_code == 0 && second_code == 1003) || (first_code == 1003 && second_code == 0));
    }
    {
        Db reopened(file);
        REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        const auto saved = EventService(reopened).detail(persisted).data;
        CHECK(saved["disposition"] == "acknowledged" || saved["disposition"] == "ignored");
        CHECK(saved["disposition_revision"].asInt64() == 1);
        REQUIRE(reopened.exec("ALTER TABLE event_disposition_history DROP COLUMN operator_name;"
                              "ALTER TABLE event_disposition_history DROP COLUMN note;"
                              "UPDATE schema_meta SET version=10;"));
        REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        const auto old_history = EventService(reopened).disposition_history(persisted).data["entries"];
        REQUIRE(old_history.size() == 1);
        CHECK(old_history[0]["to_status"] == saved["disposition"]);
        CHECK(old_history[0]["changed_at"].asInt64() > 0);
        CHECK(old_history[0]["operator_name"].isNull() && old_history[0]["note"].isNull());
    }
    unlink(file);
}
}
int main() {
    try { run(); test_disposition(); test_disposition_records(); }
    catch (const std::exception& error) { fprintf(stderr,"event_detail: %s\n",error.what()); return 1; }
    if (!g_fail) printf("event_detail PASSED\n");
    return g_fail;
}
