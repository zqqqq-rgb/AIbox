#include "task_service.h"
#include "camera_service.h"
#include "config_compiler.h"
#include "resource_budget.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
namespace aibox {
namespace biz {
namespace {
std::string dump(const Json::Value& value) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, value);
}
Json::Value parse(const std::string& text) {
    Json::Value value;
    Json::CharReaderBuilder b;
    b["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    std::string error;
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &error))
        return {};
    return value;
}
bool fields(const Json::Value& value, std::initializer_list<const char*> allowed) {
    if (!value.isObject())
        return false;
    std::set<std::string> names;
    for (auto name : allowed)
        names.insert(name);
    for (const auto& name : value.getMemberNames())
        if (!names.count(name))
            return false;
    return true;
}
TaskResult fail(int code, const std::string& message) {
    TaskResult result;
    result.code = code;
    result.message = message;
    result.data["accepted"] = false;
    return result;
}
bool supported(const std::string& cap) {
    return cap == "region_in" || cap == "dwell";
}
bool decode_draft(const Json::Value& body, AlgorithmTask& task, std::string& error) {
    auto bad = [&](const char* text) {
        error = text;
        return false;
    };
    if (!fields(body, {"name", "enabled", "camera_ids", "algorithms", "rules", "schedules",
                       "revision"}) ||
        !body["name"].isString() || body["name"].asString().empty() ||
        body["name"].asString().size() > 128 ||
        (body.isMember("enabled") && !body["enabled"].isBool()))
        return bad("invalid task fields");
    task.name = body["name"].asString();
    task.enabled = body.get("enabled", false).asBool();
    if (!body["camera_ids"].isArray() || body["camera_ids"].empty() ||
        body["camera_ids"].size() > 64)
        return bad("expected 1..64 camera IDs");
    std::set<int64_t> cameras;
    for (const auto& value : body["camera_ids"]) {
        if (!value.isInt64() || value.asInt64() <= 0 || value.asInt64() > UINT32_MAX ||
            !cameras.insert(value.asInt64()).second)
            return bad("invalid or duplicate camera ID");
        task.camera_ids.push_back(value.asInt64());
    }
    if (!body["algorithms"].isArray() || body["algorithms"].empty() ||
        body["algorithms"].size() > 64)
        return bad("expected 1..64 algorithms");
    std::map<std::string, TaskAlgorithm> algorithms;
    for (const auto& a : body["algorithms"]) {
        if (!fields(a, {"algo_id", "threshold", "fps", "enabled"}) || !a["algo_id"].isString() ||
            a["algo_id"].asString().empty() || a["algo_id"].asString().size() > 128 ||
            (a.isMember("threshold") && !a["threshold"].isNumeric()) ||
            (a.isMember("fps") && !a["fps"].isInt()) ||
            (a.isMember("enabled") && !a["enabled"].isBool()))
            return bad("invalid task algorithm fields");
        TaskAlgorithm algorithm;
        algorithm.algo_id = a["algo_id"].asString();
        algorithm.threshold = a.get("threshold", 0.45).asDouble();
        algorithm.fps = a.get("fps", 3).asInt();
        algorithm.enabled = a.get("enabled", true).asBool();
        if (!std::isfinite(algorithm.threshold) || algorithm.threshold < 0 ||
            algorithm.threshold > 1 || algorithm.fps < 1 || algorithm.fps > 25 ||
            !algorithms.emplace(algorithm.algo_id, algorithm).second)
            return bad("invalid or duplicate task algorithm");
        task.algorithms.push_back(algorithm);
    }
    if (body.isMember("rules") && (!body["rules"].isArray() || body["rules"].size() > 256))
        return bad("expected at most 256 rules");
    std::set<std::string> keys;
    for (const auto& r : body["rules"]) {
        if (!fields(r, {"key", "algo_id", "capability_id", "roi", "params", "enabled"}) ||
            !r["key"].isString() || r["key"].asString().empty() ||
            r["key"].asString().size() > 64 || !r["algo_id"].isString() ||
            !r["capability_id"].isString() || (r.isMember("enabled") && !r["enabled"].isBool()))
            return bad("invalid task rule fields");
        TaskRule rule;
        rule.key = r["key"].asString();
        rule.algo_id = r["algo_id"].asString();
        rule.capability_id = r["capability_id"].asString();
        rule.enabled = r.get("enabled", true).asBool();
        if (!keys.insert(rule.key).second || !algorithms.count(rule.algo_id))
            return bad("duplicate rule key or missing algorithm");
        if (!supported(rule.capability_id))
            return bad("rule template is not validated in this implementation");
        if (!r["roi"].isArray() || r["roi"].size() > 128)
            return bad("rule ROI must be an array of at most 128 points");
        auto params = r.get("params", Json::Value(Json::objectValue));
        if (!fields(params, {"event_type", "dwell_sec", "debounce_sec"}))
            return bad("unsupported rule parameters");
        if (params.isMember("event_type") &&
            (!params["event_type"].isString() || params["event_type"].asString().size() > 128))
            return bad("event_type must be a string of at most 128 bytes");
        if (!params.isMember("debounce_sec"))
            params["debounce_sec"] = 0; // V2 0 inherits the explicit default.
        rule.roi_json = dump(r["roi"]);
        rule.params_json = dump(params);
        const auto& a = algorithms.at(rule.algo_id);
        CamAlgoRow check;
        check.algo_id = a.algo_id;
        check.threshold = a.threshold;
        check.detect_fps = a.fps;
        check.roi_json = rule.roi_json;
        auto expanded = params;
        expanded["rule_type"] = rule.capability_id;
        check.rule_json = dump(expanded);
        proto::SetAlgos compiled;
        if (!compile_algos(1, {check}, compiled, error))
            return false;
        task.rules.push_back(rule);
    }
    if (body.isMember("schedules") &&
        (!body["schedules"].isArray() || body["schedules"].size() > 64))
        return bad("expected at most 64 schedules");
    for (const auto& s : body["schedules"]) {
        if (!fields(s, {"timezone", "weekdays", "start_minute", "end_minute"}) ||
            !s["timezone"].isString() || !s["weekdays"].isInt() || !s["start_minute"].isInt() ||
            !s["end_minute"].isInt())
            return bad("invalid schedule fields");
        TaskSchedule schedule{s["timezone"].asString(), s["weekdays"].asInt(),
                              s["start_minute"].asInt(), s["end_minute"].asInt()};
        if (!valid_task_schedule(schedule))
            return bad("invalid schedule; only UTC and fixed UTC offsets are supported");
        task.schedules.push_back(schedule);
    }
    return true;
}
} // namespace
Json::Value TaskService::capabilities() {
    Json::Value rows(Json::arrayValue);
    for (const auto& c : db_.list_capabilities()) {
        Json::Value value;
        value["id"] = c.id;
        value["name"] = c.name;
        value["rule_type"] = c.rule_type;
        value["version"] = c.version;
        value["draft_supported"] = supported(c.rule_type);
        value["runtime_supported"] = supported(c.rule_type) && runtime_;
        value["status"] =
            supported(c.rule_type) ? (runtime_ ? "available" : "blocked") : "unsupported";
        value["reason"] =
            supported(c.rule_type)
                ? (runtime_ ? "activation requires engine capability and peak resource admission"
                            : "runtime executor not attached")
                : "rule implementation not validated";
        rows.append(value);
    }
    return rows;
}
Json::Value TaskService::describe(const AlgorithmTask& task) {
    Json::Value result;
    result["id"] = static_cast<Json::Int64>(task.id);
    result["name"] = task.name;
    result["kind"] = task.kind;
    result["revision"] = static_cast<Json::Int64>(task.revision);
    result["enabled"] = task.enabled;
    result["status"] = task.deleted_at ? "deleted" : task.kind == "legacy" ? "legacy" : "draft";
    result["runtime_supported"] = task.kind == "legacy";
    result["applied"] = task.kind == "legacy" ? Json::Value() : Json::Value(false);
    result["reason"] =
        task.kind == "legacy"
            ? "runtime status is reported by camera configuration"
            : "V2 durable task activation and scheduled reconciliation not integrated";
    if (task.kind == "v2") {
        result["runtime_supported"] = runtime_ != nullptr;
        result["schedule_runtime_supported"] = runtime_ != nullptr;
        result["reason"] = runtime_ ? "runtime follows camera operation acknowledgements"
                                    : "runtime executor not attached";
        const auto at = runtime_ ? runtime_->current_time() : task_now_ms();
        const bool window = task_window_matches(task, at);
        result["schedule_active"] = window;
        result["deployments"] = Json::Value(Json::arrayValue);
        bool applied = runtime_ && runtime_->runtime_online();
        bool any = false, suspended = false, active = false;
        for (const auto& deployment : db_.task_deployments(task.id)) {
            any = true;
            CameraConfigState state;
            CameraRow camera;
            const bool known = db_.get_camera_config_state(deployment.camera_id, state) &&
                               db_.get_camera(deployment.camera_id, camera);
            bool confirmed =
                known && state.applied_revision >= deployment.target_revision &&
                state.desired_revision == state.applied_revision &&
                (state.apply_status == "succeeded" || state.apply_status == "legacy_inactive");
            if (confirmed && task.enabled && !task.schedules.empty() && !state.deleted_at) {
                CameraConfigSnapshot expected;
                ConfigOperationRow current;
                std::string error;
                confirmed = build_camera_config(db_, camera, db_.list_camera_algos(camera.id),
                                                false, expected, error, nullptr, at) &&
                            db_.get_current_config_operation(camera.id, current) &&
                            current.target_config_json == encode_camera_config(expected);
            }
            applied = applied && confirmed;
            if (known && camera.enabled && !state.deleted_at)
                active = true;
            else
                suspended = true;
            Json::Value row;
            row["camera_id"] = Json::Int64(deployment.camera_id);
            row["operation_id"] = deployment.operation_id;
            row["target_revision"] = Json::Int64(deployment.target_revision);
            row["task_revision"] = Json::Int64(deployment.task_revision);
            row["configuration_status"] = known ? state.apply_status : "unknown";
            row["desired_revision"] = Json::Int64(state.desired_revision);
            row["applied_revision"] = Json::Int64(state.applied_revision);
            ConfigOperationRow current;
            if (db_.get_current_config_operation(deployment.camera_id, current))
                row["current_operation_id"] = current.operation_id;
            row["applied"] = confirmed && runtime_ && runtime_->runtime_online();
            row["camera_enabled"] = known && camera.enabled && !state.deleted_at;
            result["deployments"].append(row);
        }
        result["applied"] = applied && any;
        result["status"] = task.deleted_at ? "deleted"
                           : !any          ? "draft"
                           : !applied      ? "pending"
                           : !task.enabled ? "disabled"
                           : !active       ? "suspended"
                           : !window       ? "waiting_schedule"
                           : suspended     ? "partially_suspended"
                                           : "running";
    }
    if (task.kind == "legacy") {
        CameraRow camera;
        CameraConfigState state;
        if (db_.get_camera(task.legacy_camera_id, camera) &&
            db_.get_camera_config_state(task.legacy_camera_id, state)) {
            result["camera_enabled"] = camera.enabled;
            result["camera_deleted"] = state.deleted_at != 0;
            result["configuration_status"] = state.apply_status;
            result["desired_revision"] = static_cast<Json::Int64>(state.desired_revision);
            result["applied_revision"] = static_cast<Json::Int64>(state.applied_revision);
        }
    }
    result["camera_ids"] = Json::Value(Json::arrayValue);
    for (auto cam : task.camera_ids)
        result["camera_ids"].append(static_cast<Json::Int64>(cam));
    result["algorithms"] = Json::Value(Json::arrayValue);
    for (const auto& a : task.algorithms) {
        Json::Value row;
        row["algo_id"] = a.algo_id;
        row["threshold"] = a.threshold;
        row["fps"] = a.fps;
        row["enabled"] = a.enabled;
        result["algorithms"].append(row);
    }
    result["rules"] = Json::Value(Json::arrayValue);
    for (const auto& r : task.rules) {
        Json::Value row;
        row["id"] = static_cast<Json::Int64>(r.id);
        row["version"] = static_cast<Json::Int64>(r.version);
        row["key"] = r.key;
        row["algo_id"] = r.algo_id;
        row["capability_id"] = r.capability_id;
        row["roi"] = parse(r.roi_json);
        row["params"] = parse(r.params_json);
        if (task.kind == "v2" && row["params"].isObject())
            row["params"].removeMember("rule_type");
        row["enabled"] = r.enabled;
        result["rules"].append(row);
    }
    result["schedules"] = Json::Value(Json::arrayValue);
    for (const auto& s : task.schedules) {
        Json::Value row;
        row["timezone"] = s.timezone;
        row["weekdays"] = s.weekdays;
        row["start_minute"] = s.start_minute;
        row["end_minute"] = s.end_minute;
        result["schedules"].append(row);
    }
    return result;
}
Json::Value TaskService::list() {
    Json::Value rows(Json::arrayValue);
    for (const auto& task : db_.list_tasks())
        rows.append(describe(task));
    return rows;
}
TaskResult TaskService::get(int64_t id) {
    AlgorithmTask task;
    if (!db_.get_task(id, task) || task.deleted_at)
        return fail(1002, "task not found");
    return {0, "ok", describe(task)};
}
TaskResult TaskService::save(int64_t id, const Json::Value& body) {
    AlgorithmTask task;
    std::string error;
    if (!decode_draft(body, task, error))
        return fail(1001, error);
    if (task.enabled)
        return fail(1003, "use the task enabled endpoint for runtime activation");
    int64_t revision = 0;
    if (id) {
        AlgorithmTask previous;
        if (!db_.get_task(id, previous) || previous.deleted_at)
            return fail(1002, "task not found");
        if (previous.kind != "v2")
            return fail(1003, "edit legacy tasks through the camera algorithms API");
        if (previous.enabled || !db_.task_runtime_settled(id))
            return fail(1003, "disable task and await runtime confirmation before editing");
        if (!body["revision"].isInt64() || body["revision"].asInt64() <= 0)
            return fail(1001, "expected positive task revision");
        revision = body["revision"].asInt64();
        if (previous.revision != revision)
            return fail(1003, "task revision conflict");
    } else if (body.isMember("revision") &&
               (!body["revision"].isInt64() || body["revision"].asInt64() != 0))
        return fail(1001, "new task revision must be zero");
    for (auto cam : task.camera_ids) {
        CameraRow camera;
        CameraConfigState state;
        if (!db_.get_camera(cam, camera) || !db_.get_camera_config_state(cam, state) ||
            state.deleted_at)
            return fail(1002, "camera missing or deleted");
    }
    task.id = id;
    if (!db_.save_task(task, revision)) {
        AlgorithmTask current;
        if (id && db_.get_task(id, current) && (current.revision != revision || current.deleted_at))
            return fail(1003, "task revision conflict");
        return fail(5001, "task save failed; no partial draft committed");
    }
    auto data = describe(task);
    data["saved"] = true;
    return {0, "draft saved", data};
}
TaskResult TaskService::remove(int64_t id, const Json::Value& body) {
    if (!fields(body, {"revision"}) || !body["revision"].isInt64() ||
        body["revision"].asInt64() <= 0)
        return fail(1001, "expected positive task revision");
    AlgorithmTask task;
    if (!db_.get_task(id, task) || task.deleted_at)
        return fail(1002, "task not found");
    if (task.kind != "v2")
        return fail(1003, "legacy task lifecycle follows its camera");
    if (task.enabled || !db_.task_runtime_settled(id))
        return fail(1003, "disable task and await runtime confirmation before deleting");
    if (task.revision != body["revision"].asInt64())
        return fail(1003, "task revision conflict");
    if (!db_.delete_task(id, task.revision)) {
        AlgorithmTask current;
        if (db_.get_task(id, current) && (current.revision != task.revision || current.deleted_at))
            return fail(1003, "task revision conflict");
        return fail(5001, "task delete failed");
    }
    // The committed CAS already determines this response; no follow-up read is required.
    auto data = describe(task);
    data["status"] = "deleted";
    data["revision"] = static_cast<Json::Int64>(task.revision + 1);
    data["saved"] = true;
    return {0, "draft deleted", data};
}
TaskResult TaskService::set_enabled(int64_t id, const Json::Value& body) {
    if (!fields(body, {"revision", "enabled"}) || !body["revision"].isInt64() ||
        body["revision"].asInt64() <= 0 || !body["enabled"].isBool())
        return fail(1001, "expected positive task revision and boolean enabled");
    if (!runtime_)
        return fail(1003, "runtime executor not attached");
    const auto transition =
        runtime_->set_task_enabled(id, body["revision"].asInt64(), body["enabled"].asBool());
    Json::Value data(Json::objectValue);
    AlgorithmTask task;
    if (db_.get_task(id, task))
        data = describe(task);
    data["accepted"] = transition.accepted;
    data["pending"] = transition.accepted && transition.code == 1003 &&
                      std::none_of(transition.cameras.begin(), transition.cameras.end(),
                                   [](const ConfigurationResult& camera) {
                                       return camera.apply_status == "failed" ||
                                              camera.apply_status == "superseded";
                                   });
    data["submitted_revision"] = Json::Int64(transition.revision);
    data["operations"] = Json::Value(Json::arrayValue);
    for (const auto& operation : transition.cameras) {
        Json::Value row;
        row["camera_id"] = Json::Int64(operation.camera_id);
        row["operation_id"] = operation.operation_id;
        row["target_revision"] = Json::Int64(operation.revision);
        row["apply_status"] = operation.apply_status;
        row["error_code"] = operation.code;
        data["operations"].append(row);
    }
    return {transition.code, transition.message, data};
}
TaskResult TaskService::preview(const Json::Value& body) {
    if (!fields(body, {"camera_id", "at_ms", "include_drafts"}) || !body["camera_id"].isInt64() ||
        body["camera_id"].asInt64() <= 0 ||
        (body.isMember("include_drafts") && !body["include_drafts"].isBool()) ||
        (body.isMember("at_ms") && (!body["at_ms"].isInt64() || body["at_ms"].asInt64() < 0 ||
                                    body["at_ms"].asInt64() > 253402300799999LL)))
        return fail(1001, "invalid preview fields");
    const auto id = body["camera_id"].asInt64();
    CameraRow camera;
    CameraConfigState state;
    if (!db_.get_camera(id, camera) || !db_.get_camera_config_state(id, state) || state.deleted_at)
        return fail(1002, "camera missing or deleted");
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto at = body.isMember("at_ms") ? body["at_ms"].asInt64() : static_cast<int64_t>(now);
    const auto tasks = db_.list_tasks();
    auto plan = compile_task_plan(tasks, id, at, body.get("include_drafts", false).asBool());
    auto& models = plan.models;
    auto& errors = plan.errors;
    auto& excluded = plan.excluded_by_schedule;
    for (const auto& task : tasks) {
        if (std::find(task.camera_ids.begin(), task.camera_ids.end(), id) == task.camera_ids.end())
            continue;
        if (!task.enabled && !(body.get("include_drafts", false).asBool() && task.kind == "v2"))
            continue;
        for (auto associated : task.camera_ids) {
            CameraRow target;
            CameraConfigState s;
            if (!db_.get_camera(associated, target) ||
                !db_.get_camera_config_state(associated, s) || s.deleted_at)
                errors.push_back("task " + std::to_string(task.id) +
                                 " references a missing or deleted camera");
        }
    }
    Json::Value data;
    data["camera_id"] = static_cast<Json::Int64>(id);
    data["at_ms"] = static_cast<Json::Int64>(at);
    data["applied"] = false;
    data["apply_supported"] = false;
    data["estimated_only"] = true;
    data["camera_enabled"] = camera.enabled;
    data["blocking_reason"] = "preview only; use task enabled endpoint for runtime admission";
    data["include_drafts"] = body.get("include_drafts", false).asBool();
    data["schedule_evaluation_only"] = true;
    proto::SetAlgos wire;
    std::string wire_error;
    const bool wire_valid = static_cast<uint64_t>(id) <= UINT32_MAX &&
                            compile_task_wire(plan, static_cast<uint32_t>(id), wire, wire_error);
    data["required_rule_config_version"] = 2;
    data["wire_valid"] = wire_valid;
    data["engine_capabilities_checked"] = false;
    if (!wire_valid && errors.empty())
        errors.push_back(wire_error.empty() ? "camera ID exceeds engine protocol range"
                                            : wire_error);
    data["models"] = Json::Value(Json::arrayValue);
    data["errors"] = Json::Value(Json::arrayValue);
    data["excluded_by_schedule"] = excluded;
    PrecheckInput budget;
    budget.focus_cam = static_cast<int>(id);
    budget.npu_cores = cores_;
    budget.npu_mem_total_kb = memory_kb_;
    for (const auto& meta : db_.list_algorithms_meta())
        if (!meta.disabled)
            budget.models.push_back({meta.algo_id, meta.npu_cost_ms, meta.npu_mem_kb,
                                     meta.trigger_type == "triggered"});
    for (const auto& entry : models)
        data["models"].append(entry.second);
    std::map<std::pair<int64_t, std::string>, int> usages;
    for (const auto& task : tasks) {
        if (!task.enabled && !(body.get("include_drafts", false).asBool() && task.kind == "v2"))
            continue;
        if (std::any_of(task.schedules.begin(), task.schedules.end(),
                        [](const TaskSchedule& s) { return !valid_task_schedule(s); })) {
            errors.push_back("invalid schedule in budget plan");
            continue;
        }
        for (auto cam : task.camera_ids) {
            CameraRow target;
            CameraConfigState target_state;
            if (!db_.get_camera(cam, target) || !db_.get_camera_config_state(cam, target_state) ||
                target_state.deleted_at || !target.enabled)
                continue;
            for (const auto& a : task.algorithms)
                if (a.enabled)
                    usages[{cam, a.algo_id}] = std::max(usages[{cam, a.algo_id}], a.fps);
        }
    }
    // Retain uncertain old workloads just as the live admission service does.
    for (const auto& cam : db_.list_cameras(false, true)) {
        CameraConfigState s;
        db_.get_camera_config_state(cam.id, s);
        auto snapshots = db_.reserved_config_snapshots(cam.id);
        if (!s.applied_config_json.empty())
            snapshots.push_back(s.applied_config_json);
        for (const auto& text : snapshots) {
            if (text.empty())
                continue;
            CameraConfigSnapshot snapshot;
            if (!decode_camera_config(text, snapshot)) {
                errors.push_back("invalid resource reservation snapshot");
                continue;
            }
            if (snapshot.deleted || !snapshot.camera.enabled)
                continue;
            if (snapshot.has_v2) {
                for (const auto& a : snapshot.runtime.algos)
                    usages[{cam.id, a.algo}] = std::max(usages[{cam.id, a.algo}], int(a.fps));
                continue;
            }
            for (const auto& a : snapshot.algos)
                if (a.enabled)
                    usages[{cam.id, a.algo_id}] =
                        std::max(usages[{cam.id, a.algo_id}], a.detect_fps);
        }
    }
    for (const auto& entry : usages)
        budget.usages.push_back(
            {entry.first.second, static_cast<int>(entry.first.first), entry.second});
    const auto estimate = npu_precheck(budget);
    data["budget"]["ok"] = estimate.ok;
    data["budget"]["reservation_policy"] = "all_enabled_peak";
    data["budget"]["code"] = estimate.code;
    data["budget"]["message"] = estimate.msg;
    data["budget"]["compute_pct"] = estimate.compute_pct;
    data["budget"]["mem_pct"] = estimate.mem_pct;
    data["budget"]["assumed_steady_cores"] = budget.npu_cores;
    data["budget"]["assumed_memory_kb"] = static_cast<Json::Int64>(budget.npu_mem_total_kb);
    for (const auto& error : errors)
        data["errors"].append(error);
    data["valid"] = errors.empty();
    return {0, "preview only; no configuration submitted", data};
}
} // namespace biz
} // namespace aibox
