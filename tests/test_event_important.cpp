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
Json::Value change(bool important, int64_t revision) {
    Json::Value body(Json::objectValue);
    body["important"] = important; body["revision"] = Json::Int64(revision);
    return body;
}
int64_t history_count(Db& db) {
    auto row = db.prepare("SELECT COUNT(*) FROM event_importance_history;");
    REQUIRE(row && row->step()); return row->i64(0);
}
void run() {
    Db db(":memory:"); REQUIRE(db.migrate() == Db::kSchemaLatest);
    EventRow event; event.camera_id = 3; event.algo_id = "person"; event.type = "DWELL";
    event.targets_json = "[]"; event.task_id = 7; event.rule_id = 6;
    event.task_revision = 2; event.rule_version = 1;
    const auto id = db.insert_event(event); REQUIRE(id > 0);
    EventService service(db);
    const auto original = service.detail(id).data;
    CHECK(!original["important"].asBool() && original["important_revision"].asInt64() == 0);
    CHECK(service.set_important(0, change(true, 0)).code == 1001);
    CHECK(service.set_important(-1, change(true, 0)).code == 1001);
    CHECK(service.set_important(999, change(true, 0)).code == 1002);
    for (const auto& invalid : {Json::Value(), Json::Value(Json::arrayValue), change(true, -1)})
        CHECK(service.set_important(id, invalid).code == 1001);
    auto bad = change(true, 0); bad["important"] = 1;
    CHECK(service.set_important(id, bad).code == 1001);
    bad = change(true, 0); bad["revision"] = "0";
    CHECK(service.set_important(id, bad).code == 1001);
    bad["revision"] = 0.0;
    CHECK(service.set_important(id, bad).code == 1001);
    bad["revision"] = Json::UInt64(UINT64_MAX);
    CHECK(service.set_important(id, bad).code == 1001);
    bad = change(true, 0); bad["unknown"] = true;
    CHECK(service.set_important(id, bad).code == 1001);
    auto result = service.set_important(id, change(false, 0));
    CHECK(result.code == 0 && !result.data["changed"].asBool());
    CHECK(history_count(db) == 0);
    result = service.set_important(id, change(true, 0));
    REQUIRE(result.code == 0);
    CHECK(result.data["important"].asBool() && result.data["important_revision"].asInt64() == 1);
    CHECK(result.data["changed"].asBool() && history_count(db) == 1);
    auto listed = db.list_events(3, "person", 0, 0, 10);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].important && listed[0].important_revision == 1);
    // Stale clients cannot overwrite a newer flag, including stale repeats.
    CHECK(service.set_important(id, change(false, 0)).code == 1003);
    CHECK(service.set_important(id, change(true, 0)).code == 1003);
    result = service.set_important(id, change(true, 1));
    CHECK(result.code == 0 && !result.data["changed"].asBool() && history_count(db) == 1);
    result = service.set_important(id, change(false, 1));
    CHECK(result.code == 0 && !result.data["important"].asBool());
    CHECK(result.data["important_revision"].asInt64() == 2 && history_count(db) == 2);
    auto after = service.detail(id).data;
    after["important_revision"] = original["important_revision"];
    CHECK(after == original); // Detection data and historical source are unchanged.

    // Failure to persist history must roll back the flag and revision as well.
    REQUIRE(db.exec("CREATE TRIGGER fail_importance BEFORE INSERT ON event_importance_history "
                    "BEGIN SELECT RAISE(ABORT,'history unavailable'); END;"));
    CHECK(service.set_important(id, change(true, 2)).code == 5001);
    CHECK(!service.detail(id).data["important"].asBool());
    CHECK(service.detail(id).data["important_revision"].asInt64() == 2 && history_count(db) == 2);
    REQUIRE(db.exec("DROP TRIGGER fail_importance;"));
    // Deferred failure at COMMIT is also atomic.
    REQUIRE(db.exec("CREATE TABLE importance_commit_probe(parent INTEGER REFERENCES events(id) "
                    "DEFERRABLE INITIALLY DEFERRED);"
                    "CREATE TRIGGER fail_importance_commit AFTER INSERT ON event_importance_history "
                    "BEGIN INSERT INTO importance_commit_probe VALUES(999999); END;"));
    CHECK(service.set_important(id, change(true, 2)).code == 5001);
    CHECK(!service.detail(id).data["important"].asBool() && history_count(db) == 2);
    REQUIRE(db.exec("DROP TRIGGER fail_importance_commit;"));
    REQUIRE(db.exec("UPDATE events SET important_revision=9223372036854775807;"));
    CHECK(service.set_important(id, change(true, INT64_MAX)).code == 1003);
    CHECK(!service.detail(id).data["important"].asBool());
    Db unavailable(":memory:");
    CHECK(EventService(unavailable).set_important(id, change(true, 0)).code == 5001);
}
void migration_and_concurrency() {
    char file[] = "/tmp/aibox_event_important_XXXXXX";
    const int fd = mkstemp(file); REQUIRE(fd >= 0); close(fd);
    {
        Db old(file);
        // Minimum v8 event fixture: migration must preserve historic records.
        REQUIRE(old.exec("CREATE TABLE schema_meta(version INTEGER NOT NULL);INSERT INTO schema_meta VALUES(8);"
            "CREATE TABLE events(id INTEGER PRIMARY KEY, camera_id INTEGER,algo_id TEXT,type TEXT,ts INTEGER,"
            "snapshot_path TEXT,targets_json TEXT,reported INTEGER);"
            "CREATE TABLE event_rule_sources(event_id INTEGER PRIMARY KEY,task_id INTEGER,rule_id INTEGER,"
            "task_revision INTEGER,rule_version INTEGER);"
            "INSERT INTO events VALUES(12,3,'person','DWELL',1234,'historic.jpg','[]',1);"
            "INSERT INTO event_rule_sources VALUES(12,7,6,2,1);"));
        REQUIRE(old.migrate() == Db::kSchemaLatest && old.migrate() == Db::kSchemaLatest);
        auto detail = EventService(old).detail(12).data;
        CHECK(detail["reported"].asBool() && detail["task_id"].asInt64() == 7);
        CHECK(detail["snapshot"].asString() == "historic.jpg" && !detail["important"].asBool());
    }
    {
        Db first(file), second(file);
        std::atomic<bool> start{false};
        int a = -1, b = -1;
        std::thread one([&] { while (!start.load()) std::this_thread::yield();
            a = EventService(first).set_important(12, change(true, 0)).code; });
        std::thread two([&] { while (!start.load()) std::this_thread::yield();
            b = EventService(second).set_important(12, change(true, 0)).code; });
        start = true; one.join(); two.join();
        CHECK((a == 0 && b == 1003) || (a == 1003 && b == 0));
        CHECK(history_count(first) == 1);
    }
    {
        Db reopened(file); REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        auto detail = EventService(reopened).detail(12).data;
        CHECK(detail["important"].asBool() && detail["important_revision"].asInt64() == 1);
        CHECK(history_count(reopened) == 1);
    }
    unlink(file);
}
}
int main() {
    try { run(); migration_and_concurrency(); }
    catch (const std::exception& error) { fprintf(stderr, "event_important: %s\n", error.what()); return 1; }
    if (!g_fail) printf("event_important PASSED\n");
    return g_fail;
}
