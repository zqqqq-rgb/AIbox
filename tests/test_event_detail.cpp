#include "event_service.h"
#include "test_util.h"
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

using namespace aibox::biz;
namespace {
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
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
    }
    {
        Db reopened(file); REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        CHECK(EventService(reopened).detail(persisted).data["type"].asString() == "REGION_IN");
    }
    unlink(file);
}
}
int main() {
    try { run(); } catch (const std::exception& error) { fprintf(stderr,"event_detail: %s\n",error.what()); return 1; }
    if (!g_fail) printf("event_detail PASSED\n");
    return g_fail;
}
