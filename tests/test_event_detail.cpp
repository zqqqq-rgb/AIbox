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
    if (body["disposition"] == "acknowledged") {
        body["operator_name"] = "Test operator";
        body["operator_employee_id"] = "OP-01";
    }
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
    body["operator_employee_id"] = "  00042  ";
    body["note"] = "  人员仍在区域内\n已联系现场  ";
    for (const auto* field : {"operator_name", "operator_employee_id"}) {
        auto missing = body;
        missing.removeMember(field);
        const auto result = service.set_disposition(id, missing);
        CHECK(result.code == 1001 && result.data["reason_code"] == "operator_identity_required");
        for (const auto* blank : {"", "  ", u8" \u3000\u00a0 "}) {
            auto invalid = body;
            invalid[field] = blank;
            CHECK(service.set_disposition(id, invalid).code == 1001);
        }
    }
    CHECK(service.detail(id).data["disposition"] == "pending");
    CHECK(service.disposition_history(id).data["entries"].empty());
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
    CHECK(first[0]["operator_employee_id"] == "00042");
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

void test_event_search() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    EventService service(db);
    EventRow event;
    event.camera_id = 3;
    event.algo_id = "person";
    event.type = "person_dwell";
    event.task_id = 7; event.rule_id = 8; event.task_revision = 1; event.rule_version = 1;
    event.ts = 2000;
    const auto oldest = db.insert_event(event);
    const auto same_time = db.insert_event(event);
    const auto latest = db.insert_event(event);
    REQUIRE(oldest > 0 && same_time > oldest && latest > same_time);
    REQUIRE(service.set_disposition(latest, transition("acknowledged", 0)).code == 0);
    Json::Value flag;
    flag["important"] = true; flag["revision"] = 0;
    REQUIRE(service.set_important(latest, flag).code == 0);

    EventSearchQuery query;
    query.camera_id = 3;
    query.task_id = 7;
    query.algo_id = "person";
    query.type = "person_dwell";
    query.from_ms = query.to_ms = 2000;
    query.disposition = "acknowledged";
    query.important = true;
    auto page = service.search(query, "");
    REQUIRE(page.code == 0 && page.data["items"].size() == 1);
    CHECK(page.data["items"][0]["id"].asInt64() == latest);
    CHECK(page.data["next_cursor"].isNull());
    query.task_id = 999;
    CHECK(service.search(query, "").data["items"].empty());
    query.task_id = 7;
    query.disposition = "pending";
    query.important = false;
    query.from_ms = 1000;
    query.limit = 1;

    page = service.search(query, "");
    REQUIRE(page.code == 0 && page.data["items"].size() == 1 && page.data["next_cursor"].isString());
    CHECK(page.data["items"][0]["id"].asInt64() == same_time);
    const auto cursor = page.data["next_cursor"].asString();
    const auto new_equal_time = db.insert_event(event);
    REQUIRE(new_equal_time > latest);
    // A newly inserted event with an older timestamp would otherwise enter the next page.
    event.ts = 1500;
    const auto backdated = db.insert_event(event);
    REQUIRE(backdated > new_equal_time);
    page = service.search(query, cursor);
    REQUIRE(page.code == 0 && page.data["items"].size() == 1);
    CHECK(page.data["items"][0]["id"].asInt64() == oldest);
    CHECK(page.data["next_cursor"].isNull());
    query.limit = 20;
    CHECK(service.search(query, "").data["items"].size() == 4);

    for (const auto* bad : {"garbage", "v1:4:2000:5", "v1:9:2000:0", "v1:9:9223372036854775808:5", "v1:9:2000:5:2"})
        CHECK(service.search(query, bad).code == 1001);
    query.limit = 101;
    CHECK(service.search(query, "").code == 1001);
    query.limit = 20;
    query.from_ms = 2001;
    CHECK(service.search(query, "").code == 1001);
    Db unavailable(":memory:");
    CHECK(EventService(unavailable).search(EventSearchQuery{}, "").code == 5001);
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
        REQUIRE(reopened.exec("ALTER TABLE event_disposition_history DROP COLUMN operator_employee_id;"
                              "ALTER TABLE event_disposition_history DROP COLUMN operator_name;"
                              "ALTER TABLE event_disposition_history DROP COLUMN note;"
                              "UPDATE schema_meta SET version=10;"));
        REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        const auto old_history = EventService(reopened).disposition_history(persisted).data["entries"];
        REQUIRE(old_history.size() == 1);
        CHECK(old_history[0]["to_status"] == saved["disposition"]);
        CHECK(old_history[0]["changed_at"].asInt64() > 0);
        CHECK(old_history[0]["operator_name"].isNull() && old_history[0]["note"].isNull());
        CHECK(old_history[0]["operator_employee_id"].isNull());
        REQUIRE(reopened.exec("UPDATE event_disposition_history SET operator_name='historical operator',note='historical note';"
                              "ALTER TABLE event_disposition_history DROP COLUMN operator_employee_id;"
                              "UPDATE schema_meta SET version=11;"));
        REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        const auto migrated_history = EventService(reopened).disposition_history(persisted).data["entries"];
        REQUIRE(migrated_history.size() == 1);
        CHECK(migrated_history[0]["operator_name"] == "historical operator" && migrated_history[0]["note"] == "historical note");
        CHECK(migrated_history[0]["operator_employee_id"].isNull());
    }
    unlink(file);
}
}
int main() {
    try { run(); test_disposition(); test_disposition_records(); test_event_search(); }
    catch (const std::exception& error) { fprintf(stderr,"event_detail: %s\n",error.what()); return 1; }
    if (!g_fail) printf("event_detail PASSED\n");
    return g_fail;
}
