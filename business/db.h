// Db —— businessd 持久层（任务分解 T4.1，详设 §5.6）
//
// 设计要点：
//   - WAL + synchronous=NORMAL + foreign_keys=ON（open 即设，详设 DDL 头三行 PRAGMA）
//   - 迁移框架：schema_meta 版本表 + 有序迁移脚本数组；幂等（重复调用零副作用）
//   - 全部 SQL 经 mu_ 递归锁串行化（详设 §5.1"单写线程化"的 mutex 等价实现；
//     事务序列持同一把锁——Txn RAII 构造即 begin，析构未提交则 rollback）
//   - 行结构与 DDL 列一一对应；返回值约定：bool=执行成败（last_error 查因），
//     int64_t=新行 id（-1 失败）；查询空结果返回空容器不视为错误
//
// 测试：":memory:" 内存库（tests/test_db.cpp；迁移幂等/外键约束/CRUD）
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/NonCopyable.h"
#include "ipc/Protocol.h"  // RegistryReadyEvt（algorithms 整表同步）

struct sqlite3;
struct sqlite3_stmt;

namespace aibox {
namespace biz {

// ---- 行结构（列名与详设 §5.6 DDL 一一对应）----

struct CameraRow {
    int64_t id = 0;
    std::string name, rtsp_main, rtsp_sub;
    std::string zlm_key_main, zlm_key_sub;  // MediaServer 代理 key（重启用，详设 §5.6）
    bool enabled = true;                    // 0=逻辑删除
    bool online = false;
    int64_t last_heartbeat = 0;             // 0=无
};

struct CamAlgoRow {
    std::string algo_id;
    double threshold = 0.45;
    int detect_fps = 3;
    std::string roi_json;  // [[x,y]...] 归一化 0~1；空串=全屏
    bool enabled = true;
    // 规则参数（事件接线）：{"rule_type":"region_in","event_type":"…",
    // "dwell_sec":5,"debounce_sec":5}；空串=未配规则（仅检测不报警）
    std::string rule_json;
    // 宫格 ROI 显示（v3 列）：display 关 = 宫格不画该算法的区域虚线；
    // color 空 = overlay 默认主题色（#rrggbb，配置页颜色卡）
    int roi_display = 1;
    std::string roi_color;
};

struct EventRow {
    int64_t id = 0;
    int64_t camera_id = 0;
    std::string algo_id, type;
    int64_t ts = 0;             // Unix ms
    std::string snapshot_path;  // 快照路径（可空）
    std::string targets_json;
    bool reported = false;      // MQTT 上报成功置 1（outbox 对账，详设 §5.8）
};

struct SegmentRow {
    int64_t id = 0;
    int64_t camera_id = 0;
    std::string file_path;
    int64_t start_ts = 0;  // Unix ms
    int64_t end_ts = 0;    // 0=未知
    double duration_s = 0;
};

struct OutboxRow {
    int64_t id = 0;
    std::string topic, payload;
    int64_t created_ts = 0;
};

struct CameraConfigState {
    int64_t desired_revision = 0, applied_revision = 0;
    std::string apply_status, last_error;
    int64_t deleted_at = 0, updated_at = 0;
};

//保存目标版本、已确认版本、应用状态、错误和删除标记
struct ConfigOperationRow {
    std::string operation_id, op_type;
    int64_t camera_id = 0, target_revision = 0;
    std::string target_config_json, previous_applied_json;
    std::string status, last_error;
    int attempts = 0;
    int64_t next_retry_at = 0, created_at = 0, updated_at = 0;
};

// 算法库页/预检用的元数据（algorithms 全表投影）
struct AlgoMetaRow {
    std::string algo_id;
    std::string name, version, model_ref;
    std::string classes;       // 原 json 数组串
    std::string trigger_type;  // steady / triggered
    double npu_cost_ms = 0;    // 仅 rknn_run 口径（manifest 回填或 selftest 实测）
    int64_t npu_mem_kb = 0;
    bool disabled = false;
};

class Db : public NonCopyable {
public:
    // 最新 schema 版本（迁移脚本数组的长度；升版只改 db.cpp 的 kLatestVersion，
    // 本常量同步维护——测试/调用方断言 migrate() 返回值用它，勿硬编码）
    static constexpr int kSchemaLatest = 4;

    // path 传 ":memory:" 即内存库（单测）；否则为文件库（目录须已存在）
    explicit Db(const std::string& path);
    ~Db();

    // 迁移到最新版本；幂等可重复调用；返回最终版本号（-1 失败，last_error 查因）
    int migrate();

    // ---- 基础 ----
    bool exec(const std::string& sql);  // DDL/PRAGMA/无参 SQL（可含多条分号语句）
    std::string last_error() const;
    int64_t last_insert_rowid() const;
    int changes() const;
    // 板端环境探测（open 时回读 PRAGMA foreign_keys 落定；厂商精简版 sqlite 可能
    // 编译期 OMIT_FOREIGN_KEY 导致外键无法启用——上层据此跳过 FK 断言，
    // 应用层一致性由 delete_camera_cascade 手动级联保证）
    bool foreign_keys_supported() const { return fk_enabled_; }

    // 事务（RAII：构造 begin，析构未 commit 则 rollback；内部持 mu_）
    class Txn {
    public:
        explicit Txn(Db& d);
        ~Txn();
        Txn(const Txn&) = delete;
        Txn& operator=(const Txn&) = delete;
        bool active() const { return active_; }
        bool commit();
    private:
        Db& db_;
        std::unique_lock<std::recursive_mutex> lock_;
        bool active_ = false;
    };
    friend class Txn;

    // ---- 预编译语句（生命周期不得超出所属 Db）----
    // 生命周期与 Db 绑定（prepare 内赋 db_）；析构 finalize 防泄漏（sqlite3_close
    // 对未 finalize 语句返回 BUSY 且不关连接）
    class Stmt {
    public:
        ~Stmt();
        // 写语句执行：DONE/ROW → true；错误 → false + last_error。
        // INSERT/UPDATE/DELETE 必须用 run——step 是行游标语义，DONE 会被判为
        // "无行"静默返回 false（板上实测踩中：changes=1 却返回 -1 丢写）
        bool run();
        // 行游标：ROW → true；DONE（无更多行/单查无果）→ false 不置错误；
        // 错误 → false + last_error
        bool step();
        bool bind(int idx, int64_t v);
        bool bind(int idx, double v);
        bool bind(int idx, const std::string& v);
        bool bind(int idx, const char* v);
        bool bind_null(int idx);
        int64_t i64(int col) const;
        double dbl(int col) const;
        std::string text(int col) const;  // NULL 列返回 ""
        bool is_null(int col) const;
    private:
        friend class Db;
        Stmt() = default;
        sqlite3_stmt* st_ = nullptr;
        Db* db_ = nullptr;
    };
    std::unique_ptr<Stmt> prepare(const std::string& sql);

    // ---- cameras ----
    int64_t insert_camera(const CameraRow& r);
    bool update_camera(const CameraRow& r);  // name/rtsp/enabled 按 id 全量更新
    bool set_camera_online(int64_t id, bool online);
    bool set_camera_keys(int64_t id, const std::string& k_main, const std::string& k_sub);
    bool get_camera(int64_t id, CameraRow& out);
    std::vector<CameraRow> list_cameras(bool enabled_only);
    bool logical_delete_camera(int64_t id);  // enabled=0（详设 §5.4 API4 副作用）
    // 物理删除：先删 camera_algos 再删 cameras（外键 NO ACTION，顺序不可反）
    bool delete_camera_cascade(int64_t id);

    // ---- camera_algos ----
    bool set_camera_algos(int64_t cam, const std::vector<CamAlgoRow>& rows);  // 全量替换
    std::vector<CamAlgoRow> list_camera_algos(int64_t cam);

    // Optimistic revision check + desired camera/algorithms + immutable operation
    // snapshot in one short local transaction. No network calls under the DB lock.
    bool submit_camera_config(const CameraRow& target, const std::vector<CamAlgoRow>& algos,
                              ConfigOperationRow& operation, int64_t expected_revision);
    bool get_camera_config_state(int64_t cam, CameraConfigState& out);
    bool get_config_operation(const std::string& operation_id, ConfigOperationRow& out);

    // ---- algorithms（registry_ready 同步刷新，整表 replace，详设 §5.6）----
    bool replace_algorithms(const proto::RegistryReadyEvt& evt);
    std::vector<AlgoMetaRow> list_algorithms_meta();  // NPU 预检（§11.4.3 闸门1）

    // ---- events ----
    int64_t insert_event(const EventRow& r);
    // 事件快照回填（截图异步完成后的第二步写入；失败不影响事件本体）
    bool update_event_snapshot(int64_t event_id, const std::string& path);
    // 过滤参数取"哨兵零值"关闭：camera_id<=0 / algo_id 空 / from/to 为 0
    std::vector<EventRow> list_events(int64_t camera_id, const std::string& algo_id,
                                      int64_t from_ms, int64_t to_ms, int limit);

    // ---- segments ----
    int64_t insert_segment(const SegmentRow& r);
    std::vector<SegmentRow> list_segments(int64_t camera_id, int64_t from_ts,
                                          int64_t to_ts, int limit);
    std::vector<SegmentRow> oldest_segments(int limit);  // start_ts 升序（磁盘清理删最旧）
    bool delete_segment(int64_t id);                     // 只删行；文件 unlink 由 Janitor 做

    // ---- outbox（MQTT 断网缓存，详设 §5.6/§5.8）----
    int64_t insert_outbox(const std::string& topic, const std::string& payload);
    std::vector<OutboxRow> pending_outbox(int limit);
    bool delete_outbox(int64_t id);
    bool mark_event_reported(int64_t event_id);

private:
    bool set_camera_algos_impl(int64_t cam, const std::vector<CamAlgoRow>& rows);
    int schema_version();
    bool set_schema_version(int v);

    sqlite3* db_ = nullptr;
    mutable std::recursive_mutex mu_;  // 全部 SQL 串行化（const 查询也要锁 → mutable）
    mutable std::string err_;
    bool fk_enabled_ = false;
};

}  // namespace biz
}  // namespace aibox
