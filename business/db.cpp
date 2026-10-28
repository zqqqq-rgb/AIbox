#include "db.h"

#include <sqlite3.h>
#include <json/json.h>

#include <chrono>

#include "common/Log.h"

namespace aibox {
namespace biz {

namespace {

// 迁移脚本：下标 i = 版本 i+1。新增版本只能追加，不得改写历史（迁移纪律）
// v1 = 详设 §5.6 DDL 全量（原样照抄，含索引）
// v2 = camera_algos 加 rule_json（事件接线：规则参数落库）
// v3 = camera_algos 加 roi_display/roi_color（宫格 ROI 显示开关 + 每算法颜色卡）
constexpr int kLatestVersion = Db::kSchemaLatest;  // 唯一权威 = db.h 公开常量

const char* kMigrationV1 =
    // 详设 §5.6：cameras
    "CREATE TABLE IF NOT EXISTS cameras (\n"
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "  name        TEXT NOT NULL,\n"
    "  rtsp_main   TEXT NOT NULL,\n"
    "  rtsp_sub    TEXT NOT NULL,\n"
    "  zlm_key_main TEXT, zlm_key_sub TEXT,\n"
    "  enabled     INTEGER NOT NULL DEFAULT 1,\n"
    "  online      INTEGER NOT NULL DEFAULT 0,\n"
    "  last_heartbeat INTEGER\n"
    ");"
    // algorithms —— registry_ready 同步刷新（整表 replace）
    "CREATE TABLE IF NOT EXISTS algorithms (\n"
    "  algo_id     TEXT PRIMARY KEY,\n"
    "  name        TEXT, version TEXT, type TEXT,\n"
    "  model_ref   TEXT, classes TEXT, trigger_type TEXT, trigger_source TEXT,\n"
    "  npu_cost_ms INTEGER, npu_mem_kb INTEGER, input_size TEXT, disabled INTEGER DEFAULT 0\n"
    ");"
    "CREATE TABLE IF NOT EXISTS camera_algos (\n"
    "  camera_id   INTEGER NOT NULL REFERENCES cameras(id),\n"
    "  algo_id     TEXT NOT NULL,\n"
    "  threshold   REAL NOT NULL DEFAULT 0.45,\n"
    "  detect_fps  INTEGER NOT NULL DEFAULT 3,\n"
    "  roi_polygon TEXT,\n"
    "  enabled     INTEGER NOT NULL DEFAULT 1,\n"
    "  PRIMARY KEY (camera_id, algo_id)\n"
    ");"
    "CREATE TABLE IF NOT EXISTS events (\n"
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "  camera_id INTEGER NOT NULL, algo_id TEXT NOT NULL,\n"
    "  type TEXT NOT NULL, ts INTEGER NOT NULL,\n"
    "  snapshot_path TEXT, targets_json TEXT,\n"
    "  reported INTEGER DEFAULT 0\n"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_events_cam_ts ON events(camera_id, ts);"
    "CREATE TABLE IF NOT EXISTS segments (\n"
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "  camera_id INTEGER NOT NULL, file_path TEXT NOT NULL,\n"
    "  start_ts INTEGER NOT NULL, end_ts INTEGER, duration_s REAL\n"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_segments_cam_ts ON segments(camera_id, start_ts);"
    "CREATE TABLE IF NOT EXISTS outbox (\n"
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "  topic TEXT, payload TEXT, created_ts INTEGER\n"
    ");";

// v2：规则参数（事件接线）——ALTER 追加列，不触碰 v1 已有数据
const char* kMigrationV2 =
    "ALTER TABLE camera_algos ADD COLUMN rule_json TEXT;";

// v3：宫格 ROI 显示开关 + 每算法颜色（配置页显示选择框/颜色卡）
const char* kMigrationV3 =
    "ALTER TABLE camera_algos ADD COLUMN roi_display INTEGER NOT NULL DEFAULT 1;"
    "ALTER TABLE camera_algos ADD COLUMN roi_color TEXT;";

// v4: desired/applied revisions and durable operations. Old inactive cameras keep
// their state; migration cannot infer whether users disabled or deleted them.
const char* kMigrationV4 =
    "CREATE TABLE camera_config_state ("
    "camera_id INTEGER PRIMARY KEY REFERENCES cameras(id) ON DELETE CASCADE,"
    "desired_revision INTEGER NOT NULL DEFAULT 0 CHECK(desired_revision>=0),"
    "applied_revision INTEGER NOT NULL DEFAULT 0 CHECK(applied_revision>=0 AND applied_revision<=desired_revision),"
    "apply_status TEXT NOT NULL DEFAULT 'unknown', last_error TEXT NOT NULL DEFAULT '',"
    "deleted_at INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0);"
    "INSERT INTO camera_config_state(camera_id, apply_status) "
    "SELECT id, CASE WHEN enabled=0 THEN 'legacy_inactive' ELSE 'unknown' END FROM cameras;"
    "CREATE TRIGGER camera_config_state_insert AFTER INSERT ON cameras BEGIN "
    "INSERT INTO camera_config_state(camera_id) VALUES(NEW.id); END;"
    "CREATE TABLE config_operations ("
    "operation_id TEXT PRIMARY KEY NOT NULL, camera_id INTEGER NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,"
    "op_type TEXT NOT NULL, target_revision INTEGER NOT NULL CHECK(target_revision>0),"
    "target_config_json TEXT NOT NULL, previous_applied_json TEXT NOT NULL,"
    "status TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0,"
    "next_retry_at INTEGER NOT NULL DEFAULT 0, last_error TEXT NOT NULL DEFAULT '',"
    "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
    "UNIQUE(camera_id, target_revision));"
    "CREATE INDEX idx_config_operations_retry ON config_operations(status,next_retry_at);";

}  // namespace

// ---- open / migrate ----

Db::Db(const std::string& path) {
    const int rc = sqlite3_open_v2(
        path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        err_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return;
    }
    sqlite3_busy_timeout(db_, 3000);
    // 详设 §5.6 头三行 PRAGMA（:memory: 下 WAL 自动降级为 memory，非错误）
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA foreign_keys=ON;");
    // 板端环境探针：版本/线程模式/pragma 生效性直接落日志（现场诊断依据）
    {
        auto st = prepare("PRAGMA foreign_keys;");
        if (st && st->step()) {
            fk_enabled_ = st->i64(0) != 0;
            if (!fk_enabled_)
                LOGW("db", "foreign_keys=OFF（ 本机 sqlite 不支持外键约束，"
                           "级联一致性由应用层 delete_camera_cascade 保证）");
        }
        LOGI("db", "sqlite %s threadsafe=%d fk=%d", sqlite3_libversion(),
             sqlite3_threadsafe(), fk_enabled_ ? 1 : 0);
    }
}

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

int Db::migrate() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!db_) return -1;
    // 版本表先行（不在迁移脚本内：迁移框架自身的地基）
    if (!exec("CREATE TABLE IF NOT EXISTS schema_meta(version INTEGER NOT NULL);")) return -1;

    const int cur = schema_version();
    if (cur < 0) return -1;
    if (cur >= kLatestVersion) return cur;  // 幂等：已是最新，零副作用

    static const char* const kMigrations[] = {kMigrationV1, kMigrationV2,
                                              kMigrationV3, kMigrationV4};  // 下标 = 版本-1
    for (int v = cur + 1; v <= kLatestVersion; ++v) {
        Txn t(*this);
        if (!t.active()) return -1;
        if (!exec(kMigrations[v - 1])) return -1;  // 析构自动 rollback
        if (!set_schema_version(v)) return -1;
        if (!t.commit()) return -1;
    }
    return schema_version();
}

int Db::schema_version() {
    auto st = prepare("SELECT version FROM schema_meta LIMIT 1;");
    if (!st) return -1;
    if (!st->step()) return err_.empty() ? 0 : -1;  // 空表 = 版本 0
    return static_cast<int>(st->i64(0));
}

bool Db::set_schema_version(int v) {
    // 注意：prepare_v2 只编译首条语句——DELETE 与 INSERT 必须分开
    if (!exec("DELETE FROM schema_meta;")) return false;
    auto st = prepare("INSERT INTO schema_meta(version) VALUES(?1);");
    if (!st || !st->bind(1, static_cast<int64_t>(v))) return false;
    return st->run();
}

// ---- 基础 ----

bool Db::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!db_) {
        err_ = "db not open";
        return false;
    }
    err_.clear();
    char* msg = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        err_ = msg ? msg : sqlite3_errmsg(db_);
        sqlite3_free(msg);
        return false;
    }
    return true;
}

std::string Db::last_error() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return err_;
}

int64_t Db::last_insert_rowid() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return db_ ? sqlite3_last_insert_rowid(db_) : 0;
}

int Db::changes() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return db_ ? sqlite3_changes(db_) : 0;
}

// ---- Txn ----

Db::Txn::Txn(Db& d) : db_(d), lock_(d.mu_) {
    if (db_.db_ && !sqlite3_get_autocommit(db_.db_)) {
        db_.err_ = "nested transactions are not supported";
        return;
    }
    active_ = db_.exec("BEGIN;");
}

Db::Txn::~Txn() {
    if (active_) {
        const std::string error = db_.err_;
        db_.exec("ROLLBACK;");
        if (!error.empty()) db_.err_ = error;
    }
}

bool Db::Txn::commit() {
    if (!active_) return false;
    if (!db_.exec("COMMIT;")) return false;
    active_ = false;
    return true;
}

// ---- Stmt ----

Db::Stmt::~Stmt() {
    if (st_) sqlite3_finalize(st_);  // 防 sqlite3_close BUSY 泄漏连接
}

std::unique_ptr<Db::Stmt> Db::prepare(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    err_.clear();
    auto st = std::unique_ptr<Stmt>(new Stmt());
    st->db_ = this;
    if (!db_) {
        err_ = "db not open";
        return st;  // st->st_ 为空，bind/step 直接失败
    }
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &st->st_, nullptr);
    if (rc != SQLITE_OK) {
        err_ = sqlite3_errmsg(db_);
        if (st->st_) {
            sqlite3_finalize(st->st_);
            st->st_ = nullptr;
        }
    }
    return st;
}

bool Db::Stmt::bind(int idx, int64_t v) {
    if (!st_) return false;
    return sqlite3_bind_int64(st_, idx, v) == SQLITE_OK;
}

bool Db::Stmt::bind(int idx, double v) {
    if (!st_) return false;
    return sqlite3_bind_double(st_, idx, v) == SQLITE_OK;
}

bool Db::Stmt::bind(int idx, const std::string& v) {
    if (!st_) return false;
    return sqlite3_bind_text(st_, idx, v.c_str(), static_cast<int>(v.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Db::Stmt::bind(int idx, const char* v) {
    if (!st_) return false;
    return sqlite3_bind_text(st_, idx, v, -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Db::Stmt::bind_null(int idx) {
    if (!st_) return false;
    return sqlite3_bind_null(st_, idx) == SQLITE_OK;
}

bool Db::Stmt::run() {
    if (!st_) {
        if (db_) db_->err_ = "stmt not prepared";  // 拒绝静默失败（板上实测教训）
        return false;
    }
    db_->err_.clear();
    const int rc = sqlite3_step(st_);
    if (rc == SQLITE_DONE || rc == SQLITE_ROW) return true;
    db_->err_ = sqlite3_errmsg(db_->db_);
    return false;
}

bool Db::Stmt::step() {
    if (!st_) {
        if (db_) db_->err_ = "stmt not prepared";  // 拒绝静默失败（板上实测教训）
        return false;
    }
    db_->err_.clear();
    const int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;  // 正常结束/无行：非错误，err_ 保持空
    db_->err_ = sqlite3_errmsg(db_->db_);
    return false;
}

int64_t Db::Stmt::i64(int col) const { return sqlite3_column_int64(st_, col); }

double Db::Stmt::dbl(int col) const { return sqlite3_column_double(st_, col); }

std::string Db::Stmt::text(int col) const {
    const auto* p = sqlite3_column_text(st_, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

bool Db::Stmt::is_null(int col) const {
    return sqlite3_column_type(st_, col) == SQLITE_NULL;
}

// ---- cameras ----

int64_t Db::insert_camera(const CameraRow& r) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("INSERT INTO cameras(name, rtsp_main, rtsp_sub, enabled) "
                      "VALUES(?1, ?2, ?3, ?4);");
    if (!st) return -1;
    if (!st->bind(1, r.name) || !st->bind(2, r.rtsp_main) || !st->bind(3, r.rtsp_sub) ||
        !st->bind(4, static_cast<int64_t>(r.enabled ? 1 : 0)))
        return -1;
    if (!st->run()) return -1;
    const int64_t id = last_insert_rowid();
    if (id <= 0) {
        // step 成功但 rowid 非法：sqlite 异常行为的显式诊断（板上实测踩中，勿删）
        err_ = "insert applied but rowid invalid (rowid=" + std::to_string(id) +
               ", changes=" + std::to_string(changes()) + ")";
        return -1;
    }
    return id;
}

bool Db::update_camera(const CameraRow& r) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("UPDATE cameras SET name=?1, rtsp_main=?2, rtsp_sub=?3, enabled=?4 "
                      "WHERE id=?5;");
    if (!st) return false;
    if (!st->bind(1, r.name) || !st->bind(2, r.rtsp_main) || !st->bind(3, r.rtsp_sub) ||
        !st->bind(4, static_cast<int64_t>(r.enabled ? 1 : 0)) ||
        !st->bind(5, r.id))
        return false;
    return st->run();
}

bool Db::set_camera_online(int64_t id, bool online) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("UPDATE cameras SET online=?1 WHERE id=?2;");
    if (!st) return false;
    if (!st->bind(1, static_cast<int64_t>(online ? 1 : 0)) || !st->bind(2, id)) return false;
    return st->run();
}

bool Db::set_camera_keys(int64_t id, const std::string& k_main, const std::string& k_sub) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("UPDATE cameras SET zlm_key_main=?1, zlm_key_sub=?2 WHERE id=?3;");
    if (!st) return false;
    if (!st->bind(1, k_main) || !st->bind(2, k_sub) || !st->bind(3, id)) return false;
    return st->run();
}

bool Db::get_camera(int64_t id, CameraRow& out) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("SELECT id, name, rtsp_main, rtsp_sub, zlm_key_main, zlm_key_sub, "
                      "enabled, online, last_heartbeat FROM cameras WHERE id=?1;");
    if (!st) return false;
    if (!st->bind(1, id) || !st->step()) return false;
    out.id = st->i64(0);
    out.name = st->text(1);
    out.rtsp_main = st->text(2);
    out.rtsp_sub = st->text(3);
    out.zlm_key_main = st->text(4);
    out.zlm_key_sub = st->text(5);
    out.enabled = st->i64(6) != 0;
    out.online = st->i64(7) != 0;
    out.last_heartbeat = st->is_null(8) ? 0 : st->i64(8);
    return true;
}

std::vector<CameraRow> Db::list_cameras(bool enabled_only) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<CameraRow> rows;
    const char* sql = enabled_only
                          ? "SELECT id, name, rtsp_main, rtsp_sub, zlm_key_main, zlm_key_sub, "
                            "enabled, online, last_heartbeat FROM cameras WHERE enabled=1 ORDER BY id;"
                          : "SELECT id, name, rtsp_main, rtsp_sub, zlm_key_main, zlm_key_sub, "
                            "enabled, online, last_heartbeat FROM cameras ORDER BY id;";
    auto st = prepare(sql);
    if (!st) return rows;
    while (st->step()) {
        CameraRow r;
        r.id = st->i64(0);
        r.name = st->text(1);
        r.rtsp_main = st->text(2);
        r.rtsp_sub = st->text(3);
        r.zlm_key_main = st->text(4);
        r.zlm_key_sub = st->text(5);
        r.enabled = st->i64(6) != 0;
        r.online = st->i64(7) != 0;
        r.last_heartbeat = st->is_null(8) ? 0 : st->i64(8);
        rows.push_back(std::move(r));
    }
    return rows;
}

bool Db::logical_delete_camera(int64_t id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("UPDATE cameras SET enabled=0, online=0 WHERE id=?1;");
    if (!st) return false;
    if (!st->bind(1, id)) return false;
    return st->run();
}

bool Db::delete_camera_cascade(int64_t id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Txn t(*this);
    if (!t.active()) return false;
    for (const char* sql : {"DELETE FROM camera_algos WHERE camera_id=?1;",
                            "DELETE FROM config_operations WHERE camera_id=?1;",
                            "DELETE FROM camera_config_state WHERE camera_id=?1;"}) {
        auto st = prepare(sql);
        if (!st || !st->bind(1, id) || !st->run()) return false;
    }
    {
        auto st = prepare("DELETE FROM cameras WHERE id=?1;");
        if (!st || !st->bind(1, id) || !st->run()) return false;
    }
    return t.commit();
}

// ---- camera_algos ----

bool Db::set_camera_algos(int64_t cam, const std::vector<CamAlgoRow>& rows) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Txn t(*this);
    if (!t.active()) return false;
    if (!set_camera_algos_impl(cam, rows)) return false;
    return t.commit();
}

bool Db::set_camera_algos_impl(int64_t cam, const std::vector<CamAlgoRow>& rows) {
    {
        auto st = prepare("DELETE FROM camera_algos WHERE camera_id=?1;");
        if (!st || !st->bind(1, cam) || !st->run()) return false;
    }
    for (const auto& r : rows) {
        auto st = prepare("INSERT INTO camera_algos(camera_id, algo_id, threshold, "
                          "detect_fps, roi_polygon, enabled, rule_json, roi_display, "
                          "roi_color) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);");
        if (!st) return false;
        if (!st->bind(1, cam) || !st->bind(2, r.algo_id) || !st->bind(3, r.threshold) ||
            !st->bind(4, static_cast<int64_t>(r.detect_fps)) ||
            !st->bind(5, r.roi_json) ||
            !st->bind(6, static_cast<int64_t>(r.enabled ? 1 : 0)) ||
            !st->bind(7, r.rule_json) ||
            !st->bind(8, static_cast<int64_t>(r.roi_display ? 1 : 0)) ||
            !st->bind(9, r.roi_color))
            return false;
        if (!st->run()) return false;
    }
    return true;
}

bool Db::submit_camera_config(const CameraRow& target, const std::vector<CamAlgoRow>& algos,
                              ConfigOperationRow& operation, int64_t expected_revision) {
    Txn txn(*this);
    if (!txn.active()) return false;
    if (expected_revision < 0 || expected_revision == INT64_MAX ||
        operation.operation_id.empty() || operation.target_config_json.empty() ||
        (operation.op_type != "create" && operation.op_type != "update" &&
         operation.op_type != "enable" && operation.op_type != "disable" &&
         operation.op_type != "delete" && operation.op_type != "set_algos")) {
        err_ = "invalid configuration operation";
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto state = prepare("UPDATE camera_config_state SET desired_revision=?1, "
                         "apply_status='pending', last_error='', updated_at=?2, deleted_at=?3 "
                         "WHERE camera_id=?4 AND desired_revision=?5 AND deleted_at=0;");
    if (!state || !state->bind(1, expected_revision + 1) ||
        !state->bind(2, static_cast<int64_t>(now)) ||
        !state->bind(3, operation.op_type == "delete" ? static_cast<int64_t>(now) : int64_t{0}) ||
        !state->bind(4, target.id) || !state->bind(5, expected_revision) || !state->run())
        return false;
    if (changes() != 1) {
        err_ = "camera missing, deleted, or configuration revision conflict";
        return false;
    }
    if (!update_camera(target) || !set_camera_algos_impl(target.id, algos)) return false;
    auto st = prepare("INSERT INTO config_operations(operation_id,camera_id,op_type,"
                      "target_revision,target_config_json,previous_applied_json,created_at,updated_at) "
                      "VALUES(?1,?2,?3,?4,?5,?6,?7,?7);");
    if (!st || !st->bind(1, operation.operation_id) || !st->bind(2, target.id) ||
        !st->bind(3, operation.op_type) || !st->bind(4, expected_revision + 1) ||
        !st->bind(5, operation.target_config_json) ||
        !st->bind(6, operation.previous_applied_json) ||
        !st->bind(7, static_cast<int64_t>(now)) || !st->run() || !txn.commit()) return false;
    operation.camera_id = target.id;
    operation.target_revision = expected_revision + 1;
    operation.status = "pending";
    operation.attempts = 0;
    operation.next_retry_at = 0;
    operation.last_error.clear();
    operation.created_at = operation.updated_at = now;
    return true;
}

bool Db::get_camera_config_state(int64_t cam, CameraConfigState& out) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto st = prepare("SELECT desired_revision,applied_revision,apply_status,last_error,"
                      "deleted_at,updated_at FROM camera_config_state WHERE camera_id=?1;");
    if (!st || !st->bind(1, cam) || !st->step()) return false;
    out.desired_revision = st->i64(0);
    out.applied_revision = st->i64(1);
    out.apply_status = st->text(2);
    out.last_error = st->text(3);
    out.deleted_at = st->i64(4);
    out.updated_at = st->i64(5);
    return true;
}

bool Db::get_config_operation(const std::string& operation_id, ConfigOperationRow& out) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto st = prepare("SELECT operation_id,camera_id,op_type,target_revision,target_config_json,"
                      "previous_applied_json,status,attempts,next_retry_at,last_error,created_at,"
                      "updated_at FROM config_operations WHERE operation_id=?1;");
    if (!st || !st->bind(1, operation_id) || !st->step()) return false;
    out.operation_id = st->text(0);
    out.camera_id = st->i64(1);
    out.op_type = st->text(2);
    out.target_revision = st->i64(3);
    out.target_config_json = st->text(4);
    out.previous_applied_json = st->text(5);
    out.status = st->text(6);
    out.attempts = static_cast<int>(st->i64(7));
    out.next_retry_at = st->i64(8);
    out.last_error = st->text(9);
    out.created_at = st->i64(10);
    out.updated_at = st->i64(11);
    return true;
}

std::vector<CamAlgoRow> Db::list_camera_algos(int64_t cam) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<CamAlgoRow> rows;
    auto st = prepare("SELECT algo_id, threshold, detect_fps, roi_polygon, enabled, "
                      "rule_json, roi_display, roi_color "
                      "FROM camera_algos WHERE camera_id=?1 ORDER BY algo_id;");
    if (!st) return rows;
    if (!st->bind(1, cam)) return rows;
    while (st->step()) {
        CamAlgoRow r;
        r.algo_id = st->text(0);
        r.threshold = st->dbl(1);
        r.detect_fps = static_cast<int>(st->i64(2));
        r.roi_json = st->is_null(3) ? "" : st->text(3);
        r.enabled = st->i64(4) != 0;
        r.rule_json = st->is_null(5) ? "" : st->text(5);
        r.roi_display = st->is_null(6) ? 1 : (st->i64(6) != 0);
        r.roi_color = st->is_null(7) ? "" : st->text(7);
        rows.push_back(std::move(r));
    }
    return rows;
}

// ---- algorithms ----

bool Db::replace_algorithms(const proto::RegistryReadyEvt& evt) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Txn t(*this);
    if (!t.active()) return false;
    {
        auto st = prepare("DELETE FROM algorithms;");
        if (!st || !st->run()) return false;
    }
    for (const auto& a : evt.algos) {
        // name/type/input_size/trigger_source：engine proto 未携带（详设 §8.1
        // registry_ready 字段集），先落空串占位；后续 proto 扩展时此处同步。
        // classes：proto 已携带（manifest 白名单），序列化为 JSON 数组串；
        // 有 class_names 时升级为对象数组 [{"id":0,"name":"person"},...]（一列两用，
        // 免加列迁移；前端按元素类型兼容：数字=裸 id，对象=带名）。空 = 全类，落 "[]"
        std::string classes_json = "[]";
        if (!a.classes.empty()) {
            Json::Value carr(Json::arrayValue);
            for (const int id : a.classes) {
                // class_names 下标 = COCO id（manifest 数组形态）；越界/空 = 无名
                const std::string name =
                    (id >= 0 && static_cast<size_t>(id) < a.class_names.size())
                        ? a.class_names[static_cast<size_t>(id)] : "";
                if (!name.empty()) {
                    Json::Value oj(Json::objectValue);
                    oj["id"] = id;
                    oj["name"] = name;
                    carr.append(oj);
                } else {
                    carr.append(id);  // 无名映射保持数字形态（向后兼容旧前端）
                }
            }
            classes_json = Json::writeString(Json::StreamWriterBuilder(), carr);
        }
        auto st = prepare("INSERT INTO algorithms(algo_id, name, version, type, model_ref, "
                          "classes, trigger_type, trigger_source, npu_cost_ms, npu_mem_kb, "
                          "input_size, disabled) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);");
        if (!st) return false;
        if (!st->bind(1, a.algo_id) || !st->bind(2, a.algo_id) || !st->bind(3, a.version) ||
            !st->bind(4, "") || !st->bind(5, a.model_ref) || !st->bind(6, classes_json) ||
            !st->bind(7, a.trigger) || !st->bind(8, "") ||
            !st->bind(9, static_cast<double>(a.npu_cost_ms)) ||
            !st->bind(10, static_cast<int64_t>(a.npu_mem_kb)) || !st->bind(11, "") ||
            !st->bind(12, static_cast<int64_t>(a.disabled ? 1 : 0)))
            return false;
        if (!st->run()) return false;  // INSERT：必须 run（step 把 DONE 判为无行）
    }
    return t.commit();
}

std::vector<AlgoMetaRow> Db::list_algorithms_meta() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<AlgoMetaRow> rows;
    auto st = prepare("SELECT algo_id, name, version, model_ref, classes, trigger_type, "
                      "npu_cost_ms, npu_mem_kb, disabled "
                      "FROM algorithms ORDER BY algo_id;");
    if (!st) return rows;
    while (st->step()) {
        AlgoMetaRow r;
        r.algo_id = st->text(0);
        r.name = st->text(1);
        r.version = st->text(2);
        r.model_ref = st->text(3);
        r.classes = st->text(4);
        r.trigger_type = st->text(5);
        r.npu_cost_ms = st->dbl(6);
        r.npu_mem_kb = st->i64(7);
        r.disabled = st->i64(8) != 0;
        rows.push_back(std::move(r));
    }
    return rows;
}

// ---- events ----

int64_t Db::insert_event(const EventRow& r) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("INSERT INTO events(camera_id, algo_id, type, ts, snapshot_path, "
                      "targets_json, reported) VALUES(?1,?2,?3,?4,?5,?6,?7);");
    if (!st) return -1;
    if (!st->bind(1, r.camera_id) || !st->bind(2, r.algo_id) || !st->bind(3, r.type) ||
        !st->bind(4, r.ts) || !st->bind(5, r.snapshot_path) || !st->bind(6, r.targets_json) ||
        !st->bind(7, static_cast<int64_t>(r.reported ? 1 : 0)))
        return -1;
    if (!st->run()) return -1;
    return last_insert_rowid();
}

std::vector<EventRow> Db::list_events(int64_t camera_id, const std::string& algo_id,
                                      int64_t from_ms, int64_t to_ms, int limit) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<EventRow> rows;
    // 自动编号 ?（按出现顺序 1,2,3...）与绑定顺序严格一致——禁止固定 ?N 跳号：
    // 条件过滤缺席时 LIMIT ?5 与实际绑定序错位（板上实测踩中：LIMIT 落 NULL）
    std::string sql =
        "SELECT id, camera_id, algo_id, type, ts, snapshot_path, targets_json, reported "
        "FROM events WHERE 1=1";
    if (camera_id > 0) sql += " AND camera_id=?";
    if (!algo_id.empty()) sql += " AND algo_id=?";
    if (from_ms > 0) sql += " AND ts>=?";
    if (to_ms > 0) sql += " AND ts<=?";
    sql += " ORDER BY ts DESC LIMIT ?;";
    auto st = prepare(sql);
    if (!st) return rows;
    int idx = 1;
    if (camera_id > 0 && !st->bind(idx++, camera_id)) return rows;
    if (!algo_id.empty() && !st->bind(idx++, algo_id)) return rows;
    if (from_ms > 0 && !st->bind(idx++, from_ms)) return rows;
    if (to_ms > 0 && !st->bind(idx++, to_ms)) return rows;
    if (!st->bind(idx, static_cast<int64_t>(limit))) return rows;
    while (st->step()) {
        EventRow r;
        r.id = st->i64(0);
        r.camera_id = st->i64(1);
        r.algo_id = st->text(2);
        r.type = st->text(3);
        r.ts = st->i64(4);
        r.snapshot_path = st->is_null(5) ? "" : st->text(5);
        r.targets_json = st->is_null(6) ? "" : st->text(6);
        r.reported = st->i64(7) != 0;
        rows.push_back(std::move(r));
    }
    return rows;
}

// ---- segments ----

int64_t Db::insert_segment(const SegmentRow& r) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("INSERT INTO segments(camera_id, file_path, start_ts, end_ts, duration_s) "
                      "VALUES(?1,?2,?3,?4,?5);");
    if (!st) return -1;
    if (!st->bind(1, r.camera_id) || !st->bind(2, r.file_path) || !st->bind(3, r.start_ts) ||
        !st->bind(4, r.end_ts) || !st->bind(5, r.duration_s))
        return -1;
    if (!st->run()) return -1;
    return last_insert_rowid();
}

std::vector<SegmentRow> Db::list_segments(int64_t camera_id, int64_t from_ts, int64_t to_ts,
                                          int limit) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<SegmentRow> rows;
    std::string sql =
        "SELECT id, camera_id, file_path, start_ts, end_ts, duration_s FROM segments WHERE 1=1";
    if (camera_id > 0) sql += " AND camera_id=?";
    if (from_ts > 0) sql += " AND start_ts>=?";
    if (to_ts > 0) sql += " AND start_ts<=?";
    sql += " ORDER BY start_ts DESC LIMIT ?;";  // 自动编号 ?，绑定序=占位序（同 list_events）
    auto st = prepare(sql);
    if (!st) return rows;
    int idx = 1;
    if (camera_id > 0 && !st->bind(idx++, camera_id)) return rows;
    if (from_ts > 0 && !st->bind(idx++, from_ts)) return rows;
    if (to_ts > 0 && !st->bind(idx++, to_ts)) return rows;
    if (!st->bind(idx, static_cast<int64_t>(limit))) return rows;
    while (st->step()) {
        SegmentRow r;
        r.id = st->i64(0);
        r.camera_id = st->i64(1);
        r.file_path = st->text(2);
        r.start_ts = st->i64(3);
        r.end_ts = st->is_null(4) ? 0 : st->i64(4);
        r.duration_s = st->dbl(5);
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<SegmentRow> Db::oldest_segments(int limit) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<SegmentRow> rows;
    auto st = prepare("SELECT id, camera_id, file_path, start_ts, end_ts, duration_s "
                      "FROM segments ORDER BY start_ts ASC LIMIT ?1;");
    if (!st) return rows;
    if (!st->bind(1, static_cast<int64_t>(limit))) return rows;
    while (st->step()) {
        SegmentRow r;
        r.id = st->i64(0);
        r.camera_id = st->i64(1);
        r.file_path = st->text(2);
        r.start_ts = st->i64(3);
        r.end_ts = st->is_null(4) ? 0 : st->i64(4);
        r.duration_s = st->dbl(5);
        rows.push_back(std::move(r));
    }
    return rows;
}

bool Db::delete_segment(int64_t id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("DELETE FROM segments WHERE id=?1;");
    if (!st) return false;
    if (!st->bind(1, id)) return false;
    return st->run();
}

// ---- outbox ----

int64_t Db::insert_outbox(const std::string& topic, const std::string& payload) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("INSERT INTO outbox(topic, payload, created_ts) VALUES(?1,?2,?3);");
    if (!st) return -1;
    const auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (!st->bind(1, topic) || !st->bind(2, payload) || !st->bind(3, now)) return -1;
    if (!st->run()) return -1;
    return last_insert_rowid();
}

std::vector<OutboxRow> Db::pending_outbox(int limit) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<OutboxRow> rows;
    auto st = prepare("SELECT id, topic, payload, created_ts FROM outbox "
                      "ORDER BY id ASC LIMIT ?1;");
    if (!st) return rows;
    if (!st->bind(1, static_cast<int64_t>(limit))) return rows;
    while (st->step()) {
        OutboxRow r;
        r.id = st->i64(0);
        r.topic = st->text(1);
        r.payload = st->text(2);
        r.created_ts = st->i64(3);
        rows.push_back(std::move(r));
    }
    return rows;
}

bool Db::delete_outbox(int64_t id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("DELETE FROM outbox WHERE id=?1;");
    if (!st) return false;
    if (!st->bind(1, id)) return false;
    return st->run();
}

bool Db::mark_event_reported(int64_t event_id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("UPDATE events SET reported=1 WHERE id=?1;");
    if (!st) return false;
    if (!st->bind(1, event_id)) return false;
    return st->run();
}

bool Db::update_event_snapshot(int64_t event_id, const std::string& path) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto st = prepare("UPDATE events SET snapshot_path=?1 WHERE id=?2;");
    if (!st) return false;
    if (!st->bind(1, path) || !st->bind(2, event_id)) return false;
    return st->run();
}

}  // namespace biz
}  // namespace aibox
