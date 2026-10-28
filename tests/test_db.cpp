// T4.1 单测（§4-T4.1 断言表）：内存 SQLite 建表 / 外键约束 / 迁移幂等
// 板上实测教训：CHECK 失败后继续执行 + 裸下标访问（rows[0]）会把任何底层失败
// 放大成 SegFault 掩盖真实错误——本文件关键路径一律 REQUIRE（失败即 return +
// 打印 db.last_error()），让板上输出直接给出根因
#include <string>
#include <vector>
#include <chrono>
#include <future>
#include <unistd.h>

#include "db.h"
#include "test_util.h"

using namespace aibox;
using namespace aibox::biz;

namespace {

// 失败即返回（同函数后续断言依赖前置成立时必须用 REQUIRE，勿用 CHECK）
#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "REQUIRE FAIL %s:%d: %s\n", __FILE__, __LINE__,  \
                    #cond);                                                  \
            g_fail = 1;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

#define REQUIRE_MSG(cond, ...)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "REQUIRE FAIL %s:%d: %s  ", __FILE__, __LINE__,  \
                    #cond);                                                  \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            g_fail = 1;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

bool table_exists(Db& db, const char* name) {
    auto st = db.prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?1;");
    if (!st) return false;
    if (!st->bind(1, name)) return false;
    if (!st->step()) return false;
    return st->i64(0) > 0;
}

void test_migrate_idempotent() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    // 幂等：重复迁移返回同版本、零报错
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    CHECK(db.last_error().empty());
    // 详设 §5.6 全部表 + schema_meta
    REQUIRE(table_exists(db, "cameras"));
    REQUIRE(table_exists(db, "algorithms"));
    REQUIRE(table_exists(db, "camera_algos"));
    REQUIRE(table_exists(db, "events"));
    REQUIRE(table_exists(db, "segments"));
    REQUIRE(table_exists(db, "outbox"));
    REQUIRE(table_exists(db, "schema_meta"));
}

void test_foreign_keys() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    // 厂商精简版 sqlite 可能编译期 OMIT_FOREIGN_KEY：环境属性而非产品缺陷，
    // 探测后跳过（产品一致性由应用层 delete_camera_cascade 手动级联保证）
    if (!db.foreign_keys_supported()) {
        fprintf(stderr, "[skip] 本机 sqlite 不支持 foreign_keys，FK 用例跳过\n");
        return;
    }
    // 外键：camera_algos 引用不存在的 camera → 插入失败
    {
        auto st = db.prepare("INSERT INTO camera_algos(camera_id, algo_id) VALUES(999, 'a');");
        REQUIRE(st != nullptr);
        st->step();
        REQUIRE_MSG(!db.last_error().empty(), "外键应拒绝: %s",
                    db.last_error().c_str());  // FOREIGN KEY constraint failed
    }
    // 有父行后插入成功；父行有子行时物理删除被外键拦下
    CameraRow cam;
    cam.name = "cam";
    cam.rtsp_main = "rtsp://m";
    cam.rtsp_sub = "rtsp://s";
    const int64_t id = db.insert_camera(cam);
    REQUIRE_MSG(id > 0, "id=%lld err=%s changes=%d", static_cast<long long>(id),
                db.last_error().c_str(), db.changes());
    REQUIRE_MSG(db.set_camera_algos(id, {{"person", 0.5, 3, "", true, "", 1, ""}}),
                "last_error=%s", db.last_error().c_str());
    {
        auto st = db.prepare("DELETE FROM cameras WHERE id=" + std::to_string(id) + ";");
        REQUIRE(st != nullptr);
        st->step();
        REQUIRE_MSG(!db.last_error().empty(), "子行存在应拒删父行: %s",
                    db.last_error().c_str());
    }
    // 级联删除：先子后父成功
    REQUIRE_MSG(db.delete_camera_cascade(id), "last_error=%s", db.last_error().c_str());
    CHECK(db.last_error().empty());
    CameraRow gone;
    CHECK(!db.get_camera(id, gone));
}

void test_camera_crud() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());

    CameraRow a;
    a.name = "前门";
    a.rtsp_main = "rtsp://127.0.0.1:554/live/cam1_main";
    a.rtsp_sub = "rtsp://127.0.0.1:554/live/cam1_sub";
    const int64_t id1 = db.insert_camera(a);
    REQUIRE_MSG(id1 > 0, "id=%lld err=%s changes=%d", static_cast<long long>(id1),
                db.last_error().c_str(), db.changes());

    // get 往返
    CameraRow got;
    REQUIRE_MSG(db.get_camera(id1, got), "get_camera fail: %s",
                db.last_error().c_str());
    CHECK(got.name == "前门");
    CHECK(got.enabled && !got.online);
    CHECK(got.zlm_key_main.empty());

    // update + keys + online
    got.rtsp_main = "rtsp://new";
    REQUIRE(db.update_camera(got));
    REQUIRE(db.set_camera_keys(id1, "key/main", "key/sub"));
    REQUIRE(db.set_camera_online(id1, true));
    CameraRow after;
    REQUIRE(db.get_camera(id1, after));
    CHECK(after.rtsp_main == "rtsp://new");
    CHECK(after.zlm_key_main == "key/main" && after.zlm_key_sub == "key/sub");
    CHECK(after.online);

    // list enabled_only
    CameraRow b;
    b.name = "后门";
    b.rtsp_main = "m2";
    b.rtsp_sub = "s2";
    const int64_t id2 = db.insert_camera(b);
    REQUIRE(id2 > 0);
    CHECK(db.list_cameras(false).size() == 2);
    CHECK(db.list_cameras(true).size() == 2);
    REQUIRE(db.logical_delete_camera(id2));
    CHECK(db.list_cameras(true).size() == 1);
    CHECK(db.list_cameras(false).size() == 2);  // 逻辑删仍在表内
    CameraRow b2;
    REQUIRE(db.get_camera(id2, b2));
    CHECK(!b2.enabled && !b2.online);  // 逻辑删同时清 online
}

void test_camera_algos_replace() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    CameraRow cam;
    cam.name = "c";
    cam.rtsp_main = "m";
    cam.rtsp_sub = "s";
    const int64_t id = db.insert_camera(cam);
    REQUIRE_MSG(id > 0, "last_error=%s", db.last_error().c_str());

    REQUIRE_MSG(db.set_camera_algos(id, {{"person", 0.5, 3, "", true, "", 1, ""},
                                         {"vehicle", 0.6, 5, "[[0,0],[1,0],[1,1]]", true,
                                          R"({"rule_type":"dwell","dwell_sec":10})", 1, ""}}),
                "last_error=%s", db.last_error().c_str());
    auto rows = db.list_camera_algos(id);
    REQUIRE_MSG(rows.size() == 2, "rows=%zu last_error=%s", rows.size(),
                db.last_error().c_str());
    CHECK(rows[0].algo_id == "person" && rows[0].detect_fps == 3);
    CHECK(rows[1].algo_id == "vehicle" && rows[1].roi_json == "[[0,0],[1,0],[1,1]]");
    // 规则参数（事件接线 v2 列）：落库往返；未配 = 空串
    CHECK(rows[0].rule_json.empty());
    CHECK(rows[1].rule_json.find("\"dwell_sec\":10") != std::string::npos);
    // ROI 显示开关/颜色（v3 列）：聚合初始化未设 = 默认值（显示开、无色）
    CHECK(rows[0].roi_display == 1 && rows[0].roi_color.empty());

    // 全量替换语义：再设 1 条 → 剩 1 条；v3 列往返（关闭显示 + 自选色）
    REQUIRE(db.set_camera_algos(id, {{"person", 0.7, 1, "", true, "", 0, "#ff8800"}}));
    rows = db.list_camera_algos(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].threshold == 0.7 && rows[0].detect_fps == 1);
    CHECK(rows[0].roi_display == 0 && rows[0].roi_color == "#ff8800");
}

void test_algorithms_replace() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    proto::RegistryReadyEvt evt;
    proto::RegistryAlgo a;
    a.algo_id = "person";
    a.version = "1.0.0";
    a.model_ref = "person-v1";
    a.trigger = "steady";
    a.npu_cost_ms = 12.5f;
    a.npu_mem_kb = 10240;
    evt.algos.push_back(a);
    a.algo_id = "face";
    a.disabled = true;
    evt.algos.push_back(a);

    REQUIRE_MSG(db.replace_algorithms(evt), "last_error=%s", db.last_error().c_str());
    REQUIRE_MSG(db.replace_algorithms(evt), "last_error=%s",
                db.last_error().c_str());  // 整表 replace：重复同步不翻倍
    {
        auto st = db.prepare("SELECT count(*) FROM algorithms;");
        REQUIRE(st != nullptr);
        REQUIRE_MSG(st->step(), "last_error=%s", db.last_error().c_str());
        CHECK(st->i64(0) == 2);
    }
    {
        auto st2 = db.prepare("SELECT disabled, npu_cost_ms FROM algorithms WHERE algo_id='face';");
        REQUIRE(st2 != nullptr);
        REQUIRE_MSG(st2->step(), "face 行应存在: %s", db.last_error().c_str());
        CHECK(st2->i64(0) == 1);
        CHECK(st2->dbl(1) == 12.5);  // REAL 列存浮点 npu_cost_ms
    }
}

void test_events() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    EventRow e;
    e.camera_id = 1;
    e.algo_id = "person";
    e.type = "REGION_IN";
    e.ts = 1000;
    e.targets_json = "[]";
    const int64_t id1 = db.insert_event(e);
    REQUIRE_MSG(id1 > 0, "last_error=%s", db.last_error().c_str());
    e.ts = 2000;
    e.algo_id = "face";
    const int64_t id2 = db.insert_event(e);
    REQUIRE(id2 > id1);

    // 过滤：camera + algo + 时间窗 + limit
    CHECK(db.list_events(1, "", 0, 0, 100).size() == 2);
    CHECK(db.list_events(1, "person", 0, 0, 100).size() == 1);
    CHECK(db.list_events(1, "", 1500, 0, 100).size() == 1);
    CHECK(db.list_events(1, "", 0, 1500, 100).size() == 1);
    CHECK(db.list_events(1, "", 0, 0, 1).size() == 1);  // ts DESC → 最新一条
    CHECK(db.list_events(2, "", 0, 0, 100).empty());    // 其他通道为空

    REQUIRE(db.mark_event_reported(id1));
    auto ev = db.list_events(1, "person", 0, 0, 1);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].reported);
}

void test_segments_outbox() {
    Db db(":memory:");
    REQUIRE_MSG(db.migrate() == Db::kSchemaLatest, "last_error=%s", db.last_error().c_str());
    for (int64_t ts : {3000, 1000, 2000}) {
        SegmentRow s;
        s.camera_id = 1;
        s.file_path = "/data/mp4/seg_" + std::to_string(ts) + ".mp4";
        s.start_ts = ts;
        s.duration_s = 300.0;
        REQUIRE_MSG(db.insert_segment(s) > 0, "ts=%lld last_error=%s",
                    static_cast<long long>(ts), db.last_error().c_str());
    }
    // oldest：start_ts 升序（磁盘清理删最旧的依据）
    auto old = db.oldest_segments(2);
    REQUIRE_MSG(old.size() == 2, "old=%zu last_error=%s", old.size(),
                db.last_error().c_str());
    CHECK(old[0].start_ts == 1000 && old[1].start_ts == 2000);
    // 删除最旧行后 list 过滤
    REQUIRE(db.delete_segment(old[0].id));
    auto all = db.list_segments(1, 0, 0, 100);
    REQUIRE(all.size() == 2);
    CHECK(all[0].start_ts == 3000);  // DESC：最新在前
    CHECK(db.list_segments(1, 2500, 0, 100).size() == 1);
    CHECK(db.list_segments(9, 0, 0, 100).empty());

    // outbox：入队 → 按序出队 → 确认删除
    REQUIRE_MSG(db.insert_outbox("t/1", "p1") > 0, "last_error=%s",
                db.last_error().c_str());
    REQUIRE(db.insert_outbox("t/2", "p2") > 0);
    auto ob = db.pending_outbox(10);
    REQUIRE(ob.size() == 2);
    CHECK(ob[0].payload == "p1" && ob[1].payload == "p2");
    REQUIRE(db.delete_outbox(ob[0].id));
    ob = db.pending_outbox(10);
    REQUIRE(ob.size() == 1);
    CHECK(ob[0].payload == "p2");
    CHECK(db.pending_outbox(0).empty());  // limit 0 → 空
}

void test_transaction_isolation() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    std::promise<void> started;
    std::future<int64_t> other;
    {
        Db::Txn txn(db);
        REQUIRE(txn.active());
        CameraRow c;
        c.name = "rolled-back";
        REQUIRE(db.insert_camera(c) > 0);
        other = std::async(std::launch::async, [&] {
            started.set_value();
            CameraRow concurrent;
            concurrent.name = "independent";
            return db.insert_camera(concurrent);
        });
        started.get_future().wait();
        CHECK(other.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
        // Rollback must never include the other thread's write.
    }
    CHECK(other.get() > 0);
    auto rows = db.list_cameras(false);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == "independent");
}

void test_transaction_failures() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    {
        Db::Txn outer(db);
        REQUIRE(outer.active());
        CameraRow c;
        c.name = "outer";
        REQUIRE(db.insert_camera(c) > 0);
        {
            Db::Txn nested(db);
            CHECK(!nested.active());
            CHECK(!nested.commit());
        }
        // Rejected helpers must not run SQL in, or roll back, the outer transaction.
        CHECK(!db.set_camera_algos(1, {}));
        CHECK(!db.delete_camera_cascade(1));
        CHECK(!db.replace_algorithms({}));
        CHECK(outer.commit());
    }
    CHECK(db.list_cameras(false).size() == 1);
    {
        Db::Txn txn(db);
        REQUIRE(txn.active());
        CHECK(!db.exec("INSERT INTO nonexistent VALUES(1);"));
    }
    CHECK(db.last_error().find("nonexistent") != std::string::npos);
    // A deferred FK violation makes COMMIT fail; destructor must still roll back.
    REQUIRE(db.exec("CREATE TABLE parent(id INTEGER PRIMARY KEY);"
                    "CREATE TABLE child(id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);"));
    {
        Db::Txn txn(db);
        REQUIRE(txn.active());
        REQUIRE(db.exec("INSERT INTO child VALUES(42);"));
        CHECK(!txn.commit());
        CHECK(txn.active());
    }
    CHECK(db.last_error().find("FOREIGN KEY") != std::string::npos);
    REQUIRE(db.exec("INSERT INTO parent VALUES(42);"));
    auto st = db.prepare("SELECT count(*) FROM child;");
    REQUIRE(st && st->step());
    CHECK(st->i64(0) == 0);
}

void test_v4_migration_preserves_existing_data() {
    Db db(":memory:");
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    CameraRow cam;
    cam.name = "legacy";
    cam.enabled = false;
    const auto id = db.insert_camera(cam);
    REQUIRE(id > 0);
    REQUIRE(db.set_camera_algos(id, {{"person", 0.6, 3, "", true, "", 1, ""}}));
    EventRow event;
    event.camera_id = id;
    event.type = "retained";
    REQUIRE(db.insert_event(event) > 0);
    // Restore exactly the pre-v4 schema while preserving legacy business rows.
    REQUIRE(db.exec("DROP TRIGGER camera_config_state_insert; DROP TABLE config_operations;"
                    "DROP TABLE camera_config_state; UPDATE schema_meta SET version=3;"));
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    REQUIRE(db.migrate() == Db::kSchemaLatest);
    CameraConfigState state;
    REQUIRE(db.get_camera_config_state(id, state));
    CHECK(state.apply_status == "legacy_inactive" && state.deleted_at == 0);
    CHECK(state.desired_revision == 0 && state.applied_revision == 0);
    CameraRow restored;
    REQUIRE(db.get_camera(id, restored));
    CHECK(restored.name == "legacy" && !restored.enabled);
    CHECK(db.list_camera_algos(id).size() == 1);
    CHECK(db.list_events(id, "", 0, 0, 10).size() == 1);
}

void test_durable_config_submission() {
    const std::string path = "/tmp/aibox_config_db_" + std::to_string(::getpid()) + ".db";
    ::unlink(path.c_str());
    int64_t id = 0;
    {
        Db db(path);
        REQUIRE(db.migrate() == Db::kSchemaLatest);
        CameraRow target;
        target.name = "before";
        id = db.insert_camera(target);
        REQUIRE(id > 0);
        target.id = id;
        target.name = "desired";
        ConfigOperationRow operation;
        operation.operation_id = "op-1";
        operation.op_type = "update";
        operation.target_config_json = R"({"name":"desired","algos":["person"]})";
        operation.previous_applied_json = "{}";
        const std::vector<CamAlgoRow> algos = {{"person", 0.6, 3, "", true, "", 1, ""}};
        REQUIRE(db.submit_camera_config(target, algos, operation, 0));
        CHECK(operation.target_revision == 1 && operation.status == "pending");
        ConfigOperationRow read;
        REQUIRE(db.get_config_operation("op-1", read));
        CHECK(read.target_config_json == operation.target_config_json);
        CHECK(read.camera_id == id && read.target_revision == 1);
        CameraConfigState state;
        REQUIRE(db.get_camera_config_state(id, state));
        CHECK(state.desired_revision == 1 && state.applied_revision == 0);
        CHECK(state.apply_status == "pending");
        // Operation insert fails after camera/algorithm writes: roll all of them back.
        target.name = "must-rollback";
        CHECK(!db.submit_camera_config(target, {}, operation, 1));
        CHECK(db.last_error().find("UNIQUE") != std::string::npos);
        CameraRow current;
        REQUIRE(db.get_camera(id, current));
        CHECK(current.name == "desired");
        CHECK(db.list_camera_algos(id).size() == 1);
        REQUIRE(db.get_camera_config_state(id, state));
        CHECK(state.desired_revision == 1);
        operation.operation_id = "stale";
        CHECK(!db.submit_camera_config(target, {}, operation, 0));
        CHECK(!db.get_config_operation("stale", read));
        // Same expected version from concurrent callers: exactly one is accepted.
        auto left = operation, right = operation;
        left.operation_id = "left";
        right.operation_id = "right";
        auto a = std::async(std::launch::async, [&] {
            return db.submit_camera_config(target, algos, left, 1);
        });
        auto b = std::async(std::launch::async, [&] {
            return db.submit_camera_config(target, algos, right, 1);
        });
        const bool accepted_left = a.get(), accepted_right = b.get();
        CHECK(accepted_left != accepted_right);
        REQUIRE(db.get_camera_config_state(id, state));
        CHECK(state.desired_revision == 2 && state.applied_revision == 0);
    }
    {
        Db reopened(path);
        REQUIRE(reopened.migrate() == Db::kSchemaLatest);
        CameraConfigState state;
        REQUIRE(reopened.get_camera_config_state(id, state));
        CHECK(state.desired_revision == 2 && state.apply_status == "pending");
        ConfigOperationRow read;
        REQUIRE(reopened.get_config_operation("op-1", read));
        CHECK(read.status == "pending");
    }
    ::unlink(path.c_str());
}

void run() {
    // 构建标记：板上输出若缺此行 = 陈旧二进制（sync/编译缓存问题，勿信行号）
    fprintf(stderr, "[diag] test_db v3 (fk-probe + rowid-diag + REQUIRE guards)\n");
    test_migrate_idempotent();
    test_foreign_keys();
    test_camera_crud();
    test_camera_algos_replace();
    test_algorithms_replace();
    test_events();
    test_segments_outbox();
    test_transaction_isolation();
    test_transaction_failures();
    test_v4_migration_preserves_existing_data();
    test_durable_config_submission();
}

}  // namespace

TEST_MAIN("db")
