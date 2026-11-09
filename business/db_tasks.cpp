#include "config_compiler.h"
#include "db.h"
#include "task_compiler.h"
#include <algorithm>
#include <chrono>
#include <set>
namespace aibox {
namespace biz {
std::vector<Capability> Db::list_capabilities() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<Capability> rows;
    auto st = prepare(
        "SELECT capability_id,name,rule_type,version FROM capabilities ORDER BY capability_id;");
    if (st)
        while (st->step())
            rows.push_back({st->text(0), st->text(1), st->text(2), static_cast<int>(st->i64(3))});
    return rows;
}
bool Db::get_task(int64_t id, AlgorithmTask& out) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto st = prepare("SELECT name,kind,legacy_camera_id,enabled,revision,deleted_at FROM "
                      "algorithm_tasks WHERE id=?1;");
    if (!st || !st->bind(1, id) || !st->step())
        return false;
    AlgorithmTask result;
    result.id = id;
    result.name = st->text(0);
    result.kind = st->text(1);
    result.legacy_camera_id = st->i64(2);
    result.enabled = st->i64(3) != 0;
    result.revision = st->i64(4);
    result.deleted_at = st->i64(5);
    auto cams = prepare("SELECT camera_id FROM task_cameras WHERE task_id=?1 ORDER BY camera_id;");
    if (!cams || !cams->bind(1, id))
        return false;
    while (cams->step())
        result.camera_ids.push_back(cams->i64(0));
    auto algos = prepare("SELECT algo_id,threshold,fps,enabled,roi_display,roi_color FROM "
                         "task_algorithms WHERE task_id=?1 ORDER BY algo_id;");
    if (!algos || !algos->bind(1, id))
        return false;
    while (algos->step())
        result.algorithms.push_back({algos->text(0), algos->dbl(1), static_cast<int>(algos->i64(2)),
                                     algos->i64(3) != 0, static_cast<int>(algos->i64(4)),
                                     algos->text(5)});
    auto rules =
        prepare("SELECT id,version,rule_key,algo_id,capability_id,roi_json,params_json,enabled "
                "FROM task_rules WHERE task_id=?1 ORDER BY id;");
    if (!rules || !rules->bind(1, id))
        return false;
    while (rules->step())
        result.rules.push_back({rules->i64(0), rules->i64(1), rules->text(2), rules->text(3),
                                rules->text(4), rules->text(5), rules->text(6),
                                rules->i64(7) != 0});
    auto schedules = prepare("SELECT timezone,weekdays,start_minute,end_minute FROM task_schedules "
                             "WHERE task_id=?1 ORDER BY position;");
    if (!schedules || !schedules->bind(1, id))
        return false;
    while (schedules->step())
        result.schedules.push_back({schedules->text(0), static_cast<int>(schedules->i64(1)),
                                    static_cast<int>(schedules->i64(2)),
                                    static_cast<int>(schedules->i64(3))});
    out = std::move(result);
    return true;
}
std::vector<AlgorithmTask> Db::list_tasks() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<AlgorithmTask> result;
    auto st =
        prepare("SELECT t.id FROM algorithm_tasks t LEFT JOIN camera_config_state s ON "
                "s.camera_id=t.legacy_camera_id "
                "WHERE t.deleted_at=0 AND (t.kind!='legacy' OR s.deleted_at=0) ORDER BY t.id;");
    if (st)
        while (st->step()) {
            AlgorithmTask row;
            if (!get_task(st->i64(0), row))
                return {};
            result.push_back(std::move(row));
        }
    return result;
}
bool Db::save_task(AlgorithmTask& value, int64_t expected_revision) {
    Txn txn(*this);
    if (!txn.active())
        return false;
    if (value.id && !task_runtime_settled(value.id)) {
        err_ = "task runtime transition is still pending";
        return false;
    }
    if (value.kind != "v2" || value.enabled || value.deleted_at || value.name.empty()) {
        err_ = "only disabled V2 drafts can be saved";
        return false;
    }
    const bool created = value.id == 0;
    int64_t id = value.id;
    if (created) {
        if (expected_revision != 0) {
            err_ = "task revision conflict";
            return false;
        }
        auto st = prepare("INSERT INTO algorithm_tasks(name,kind,enabled) VALUES(?1,'v2',0);");
        if (!st || !st->bind(1, value.name) || !st->run())
            return false;
        id = last_insert_rowid();
    } else {
        auto st = prepare("UPDATE algorithm_tasks SET name=?1,revision=revision+1 WHERE id=?2 AND "
                          "revision=?3 AND kind='v2' AND enabled=0 AND deleted_at=0;");
        if (!st || !st->bind(1, value.name) || !st->bind(2, id) ||
            !st->bind(3, expected_revision) || !st->run())
            return false;
        if (changes() != 1) {
            err_ = "task missing, immutable, or revision conflict";
            return false;
        }
    }
    // A revised draft has not been deployed. Keep operation history, but detach
    // confirmations for the previous revision and its former camera bindings.
    for (const char* sql : {"DELETE FROM task_deployments WHERE task_id=?1;",
                            "DELETE FROM task_cameras WHERE task_id=?1;",
                            "DELETE FROM task_schedules WHERE task_id=?1;"}) {
        auto st = prepare(sql);
        if (!st || !st->bind(1, id) || !st->run())
            return false;
    }
    for (auto cam : value.camera_ids) {
        CameraRow camera;
        CameraConfigState state;
        if (!get_camera(cam, camera) || !get_camera_config_state(cam, state) || state.deleted_at) {
            err_ = "camera missing or deleted";
            return false;
        }
        auto st = prepare("INSERT INTO task_cameras VALUES(?1,?2);");
        if (!st || !st->bind(1, id) || !st->bind(2, cam) || !st->run())
            return false;
    }
    std::set<std::string> retained_algorithms, retained_rules;
    for (const auto& a : value.algorithms) {
        if (!retained_algorithms.insert(a.algo_id).second) {
            err_ = "duplicate task algorithm";
            return false;
        }
        auto st = prepare("INSERT INTO task_algorithms VALUES(?1,?2,?3,?4,?5,?6,?7) ON "
                          "CONFLICT(task_id,algo_id) DO UPDATE SET "
                          "threshold=excluded.threshold,fps=excluded.fps,enabled=excluded.enabled,"
                          "roi_display=excluded.roi_display,roi_color=excluded.roi_color;");
        if (!st || !st->bind(1, id) || !st->bind(2, a.algo_id) || !st->bind(3, a.threshold) ||
            !st->bind(4, static_cast<int64_t>(a.fps)) ||
            !st->bind(5, static_cast<int64_t>(a.enabled)) ||
            !st->bind(6, static_cast<int64_t>(a.roi_display)) || !st->bind(7, a.roi_color) ||
            !st->run())
            return false;
    }
    for (const auto& r : value.rules) {
        if (!retained_rules.insert(r.key).second) {
            err_ = "duplicate task rule key";
            return false;
        }
        auto st = prepare("INSERT INTO "
                          "task_rules(task_id,rule_key,algo_id,capability_id,roi_json,params_json,"
                          "enabled) VALUES(?1,?2,?3,?4,?5,?6,?7) "
                          "ON CONFLICT(task_id,rule_key) DO UPDATE SET "
                          "algo_id=excluded.algo_id,capability_id=excluded.capability_id,roi_json="
                          "excluded.roi_json,"
                          "params_json=excluded.params_json,enabled=excluded.enabled,version=task_"
                          "rules.version+1 WHERE "
                          "algo_id!=excluded.algo_id OR capability_id!=excluded.capability_id OR "
                          "roi_json!=excluded.roi_json OR params_json!=excluded.params_json OR "
                          "enabled!=excluded.enabled;");
        if (!st || !st->bind(1, id) || !st->bind(2, r.key) || !st->bind(3, r.algo_id) ||
            !st->bind(4, r.capability_id) || !st->bind(5, r.roi_json) ||
            !st->bind(6, r.params_json) || !st->bind(7, static_cast<int64_t>(r.enabled)) ||
            !st->run())
            return false;
    }
    // Remove no-longer-present rules before algorithms, preserving stable IDs for retained keys.
    for (const auto& table : {std::string("task_rules"), std::string("task_algorithms")}) {
        const bool rules = table == "task_rules";
        auto old = prepare("SELECT " + std::string(rules ? "rule_key" : "algo_id") + " FROM " +
                           table + " WHERE task_id=?1;");
        if (!old || !old->bind(1, id))
            return false;
        std::vector<std::string> removed;
        while (old->step())
            if (!(rules ? retained_rules : retained_algorithms).count(old->text(0)))
                removed.push_back(old->text(0));
        for (const auto& key : removed) {
            auto st = prepare("DELETE FROM " + table + " WHERE task_id=?1 AND " +
                              (rules ? "rule_key" : "algo_id") + "=?2;");
            if (!st || !st->bind(1, id) || !st->bind(2, key) || !st->run())
                return false;
        }
    }
    int index = 0;
    for (const auto& s : value.schedules) {
        auto st = prepare("INSERT INTO task_schedules VALUES(?1,?2,?3,?4,?5,?6);");
        if (!st || !st->bind(1, id) || !st->bind(2, static_cast<int64_t>(index++)) ||
            !st->bind(3, s.timezone) || !st->bind(4, static_cast<int64_t>(s.weekdays)) ||
            !st->bind(5, static_cast<int64_t>(s.start_minute)) ||
            !st->bind(6, static_cast<int64_t>(s.end_minute)) || !st->run())
            return false;
    }
    AlgorithmTask saved;
    if (!get_task(id, saved) || !txn.commit())
        return false;
    value = std::move(saved);
    return true;
}
bool Db::delete_task(int64_t id, int64_t expected_revision) {
    Txn txn(*this);
    if (!txn.active())
        return false;
    if (!task_runtime_settled(id)) {
        err_ = "task runtime transition is still pending";
        return false;
    }
    auto st = prepare("UPDATE algorithm_tasks SET deleted_at=?1,revision=revision+1 WHERE id=?2 "
                      "AND revision=?3 AND kind='v2' AND enabled=0 AND deleted_at=0;");
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (!st || !st->bind(1, static_cast<int64_t>(now)) || !st->bind(2, id) ||
        !st->bind(3, expected_revision) || !st->run())
        return false;
    if (changes() != 1) {
        err_ = "task missing, immutable, or revision conflict";
        return false;
    }
    return txn.commit();
}

std::vector<TaskDeployment> Db::task_deployments(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<TaskDeployment> rows;
    auto st = prepare("SELECT camera_id,task_revision,target_revision,operation_id FROM "
                      "task_deployments WHERE task_id=?1 ORDER BY camera_id;");
    if (st && st->bind(1, id))
        while (st->step())
            rows.push_back({st->i64(0), st->i64(1), st->i64(2), st->text(3)});
    return rows;
}
bool Db::task_runtime_settled(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto st = prepare(
        "SELECT count(*) FROM task_deployments d LEFT JOIN camera_config_state s ON "
        "s.camera_id=d.camera_id "
        "WHERE d.task_id=?1 AND (s.camera_id IS NULL OR s.applied_revision<d.target_revision OR "
        "s.applied_revision!=s.desired_revision OR s.apply_status NOT IN "
        "('succeeded','legacy_inactive'));");
    return st && st->bind(1, id) && st->step() && st->i64(0) == 0;
}
bool Db::submit_task_enabled(int64_t id, int64_t revision, bool enabled,
                             std::vector<ConfigOperationRow>& operations, int64_t at_ms) {
    Txn txn(*this);
    if (!txn.active())
        return false;
    AlgorithmTask task;
    if (revision <= 0 || revision == INT64_MAX || !get_task(id, task) || task.kind != "v2" ||
        task.deleted_at || task.revision != revision || task.enabled == enabled ||
        task.camera_ids.size() != operations.size()) {
        err_ = "invalid task transition or revision conflict";
        return false;
    }
    auto update = prepare(
        "UPDATE algorithm_tasks SET enabled=?1,revision=revision+1 WHERE id=?2 AND revision=?3;");
    if (!update || !update->bind(1, int64_t(enabled)) || !update->bind(2, id) ||
        !update->bind(3, revision) || !update->run() || changes() != 1)
        return false;
    // No network actions occur until the task and every camera operation commit together.
    auto clear = prepare("DELETE FROM task_deployments WHERE task_id=?1;");
    if (!clear || !clear->bind(1, id) || !clear->run())
        return false;
    auto submitted = operations;
    for (size_t index = 0; index < task.camera_ids.size(); ++index) {
        const auto cam = task.camera_ids[index];
        CameraRow camera;
        CameraConfigState state;
        if (!get_camera(cam, camera) || !get_camera_config_state(cam, state) ||
            (enabled && state.deleted_at))
            return false;
        auto& operation = submitted[index];
        if (state.deleted_at) { // A completed camera tombstone already removes this task's rules.
            ConfigOperationRow current;
            if (!get_current_config_operation(cam, current))
                return false;
            operation = current;
        } else {
            operation.op_type = "set_algos";
            operation.previous_applied_json = state.applied_config_json;
            if (!submit_camera_config_impl(camera, list_camera_algos(cam), operation,
                                           state.desired_revision, at_ms))
                return false;
        }
        auto link = prepare("INSERT INTO task_deployments VALUES(?1,?2,?3,?4,?5);");
        if (!link || !link->bind(1, id) || !link->bind(2, cam) || !link->bind(3, revision + 1) ||
            !link->bind(4, operation.target_revision) || !link->bind(5, operation.operation_id) ||
            !link->run())
            return false;
    }
    if (!txn.commit())
        return false;
    operations = std::move(submitted);
    return true;
}
bool Db::refresh_task_schedules(int64_t at_ms) {
    Txn txn(*this);
    if (!txn.active())
        return false;
    const auto tasks = list_tasks();
    std::set<int64_t> cameras;
    for (const auto& task : tasks) {
        if (task.kind != "v2" || !task.enabled || task.schedules.empty())
            continue;
        for (const auto& schedule : task.schedules)
            if (!valid_task_schedule(schedule)) {
                err_ = "invalid enabled task schedule";
                return false;
            }
        cameras.insert(task.camera_ids.begin(), task.camera_ids.end());
    }
    for (auto id : cameras) {
        CameraRow camera;
        CameraConfigState state;
        if (!get_camera(id, camera) || !get_camera_config_state(id, state))
            return false;
        if (state.deleted_at)
            continue;
        CameraConfigSnapshot target;
        if (!build_camera_config(*this, camera, list_camera_algos(id), false, target, err_, &tasks,
                                 at_ms))
            return false;
        ConfigOperationRow current;
        const auto encoded = encode_camera_config(target);
        const auto previous = get_current_config_operation(id, current) ? current.target_config_json
                                                                        : state.applied_config_json;
        if (encoded == previous)
            continue;
        ConfigOperationRow operation;
        operation.operation_id =
            "schedule-" + std::to_string(id) + "-" + std::to_string(state.desired_revision + 1);
        operation.op_type = "set_algos";
        operation.previous_applied_json = state.applied_config_json;
        if (!submit_camera_config_impl(camera, list_camera_algos(id), operation,
                                       state.desired_revision, at_ms))
            return false;
        for (const auto& task : tasks) {
            if (task.kind != "v2" || !task.enabled || task.schedules.empty() ||
                std::find(task.camera_ids.begin(), task.camera_ids.end(), id) ==
                    task.camera_ids.end())
                continue;
            auto link = prepare("INSERT INTO task_deployments VALUES(?1,?2,?3,?4,?5) ON "
                                "CONFLICT(task_id,camera_id) "
                                "DO UPDATE SET "
                                "task_revision=excluded.task_revision,target_revision=excluded."
                                "target_revision,operation_id=excluded.operation_id;");
            if (!link || !link->bind(1, task.id) || !link->bind(2, id) ||
                !link->bind(3, task.revision) || !link->bind(4, operation.target_revision) ||
                !link->bind(5, operation.operation_id) || !link->run())
                return false;
        }
    }
    return txn.commit();
}
} // namespace biz
} // namespace aibox
