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
Json::Value finding(const char* status, const std::string& code, const char* reason, const char* next = "") {
    Json::Value value;
    value["status"] = status;
    value["reason_code"] = code;
    value["reason"] = reason;
    value["next_step"] = next;
    return value;
}
Json::Value engine_issue(const EngineObservation& engine) {
    if (engine.state == "online") return {};
    if (engine.state == "engine_offline")
        return finding("blocked", engine.state, "引擎未连接。", "检查引擎服务，连接恢复后重新诊断。");
    if (engine.state == "engine_syncing")
        return finding("waiting", engine.state, "引擎正在恢复摄像头配置。", "等待同步完成后重新诊断。");
    if (engine.state == "engine_declaration_missing")
        return finding("blocked", engine.state, "当前引擎未声明模型和配置版本诊断能力。", "升级引擎后重新诊断。");
    if (engine.state == "engine_query_failed")
        return finding("blocked", engine.state, "暂时无法读取引擎状态。", "稍后重新诊断。如果仍失败，检查引擎连接。");
    return finding("blocked", engine.state, "任务执行器未启动。", "检查业务服务的启动状态。");
}
bool supports_type(const Json::Value& types, const std::string& type) {
    return types.isArray() && std::any_of(types.begin(), types.end(),
        [&](const Json::Value& value) { return value.isString() && value.asString() == type; });
}
Json::Value model_readiness(const Json::Value& model, const std::string& type) {
    if (model.isNull())
        return finding("blocked", "model_missing", "引擎未加载此算法模型。", "安装并加载对应算法包。");
    if (model["disabled"] != false)
        return finding("blocked", "model_unavailable", "算法模型未通过加载或自检。", "检查算法包和引擎加载日志。");
    if (model["v2_threshold_supported"] != true ||
        (!type.empty() && !supports_type(model["rule_types"], type)))
        return finding("blocked", "model_rule_unsupported", "模型不支持此任务所需的规则或独立阈值。", "选择支持该规则的算法模型。");
    return finding("available", "model_ready", "模型支持此任务所需的配置。");
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
    const auto engine = runtime_ ? runtime_->observe_engine() : EngineObservation{};
    const auto blocked = engine_issue(engine);
    const auto& models = engine.capabilities["models"];
    Json::Value rows(Json::arrayValue);
    for (const auto& c : db_.list_capabilities()) {
        Json::Value value;
        Json::Value model_rows(Json::arrayValue);
        const bool implemented = supported(c.rule_type);
        if (!implemented) {
            const bool calibration = c.rule_type == "speed" || c.rule_type == "ref_geom";
            value = finding("unsupported", "rule_not_implemented",
                            calibration ? "此规则需要引擎规则和场景标定支持，当前版本尚未开放。"
                                        : "当前版本尚未支持此规则。");
        } else if (!blocked.isNull()) {
            value = blocked;
            value["status"] = "blocked";
        } else if (!supports_type(engine.capabilities["rule_types"], c.rule_type)) {
            value = finding("blocked", "engine_rule_unsupported", "当前引擎未声明此规则。", "检查引擎版本与规则支持范围。");
        } else {
            bool ready = false;
            if (models.isArray()) for (const auto& model : models) {
                if (!model.isObject() || !model["algo_id"].isString()) continue;
                auto item = model_readiness(model, c.rule_type);
                item["algo_id"] = model["algo_id"];
                if (model["version"].isString()) item["version"] = model["version"];
                ready = ready || item["status"] == "available";
                model_rows.append(item);
            }
            value = ready ? finding("available", "compatible_model_ready", "当前引擎与已加载模型支持此规则。")
                          : finding("blocked", "compatible_model_missing", "当前没有可用的兼容模型。", "加载支持此规则的算法包。");
        }
        value["id"] = c.id;
        value["name"] = c.name;
        value["rule_type"] = c.rule_type;
        value["version"] = c.version;
        value["draft_supported"] = implemented;
        value["runtime_supported"] = value["status"] == "available";
        value["models"] = model_rows;
        value["simulation"] = engine.capabilities["simulation"];
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
TaskResult TaskService::diagnostics(int64_t id) {
    AlgorithmTask task;
    if (!db_.get_task(id, task) || task.deleted_at) return fail(1002, "task not found");
    if (task.kind != "v2") return fail(1003, "use camera configuration for legacy tasks");

    const auto engine = runtime_ ? runtime_->observe_engine() : EngineObservation{};
    // Collect DB observations together, without holding a database lock during IPC.
    Db::Txn read(db_);
    if (!read.active()) return fail(5001, "cannot read task diagnostics");
    if (!db_.get_task(id, task) || task.deleted_at) return fail(1002, "task not found");
    const auto view = describe(task);
    const auto unavailable = engine_issue(engine);
    const auto& models = engine.capabilities["models"];
    const auto& revisions = engine.capabilities["configuration_revisions"];
    const auto& frame_states = engine.capabilities["camera_frames"];
    const bool version_readback = engine.capabilities["configuration_revision_ack"] == true && revisions.isObject();
    const bool has_enabled_models = std::any_of(task.algorithms.begin(), task.algorithms.end(),
        [](const TaskAlgorithm& algorithm) { return algorithm.enabled; });

    Json::Value data, model_issues(Json::arrayValue), cameras(Json::arrayValue);
    data["task_id"] = Json::Int64(id);
    data["task_revision"] = Json::Int64(task.revision);
    data["engine_state"] = engine.state;
    data["simulation"] = engine.capabilities["simulation"];
    data["schedule_active"] = view["schedule_active"];
    data["observed_at"] = Json::Int64(task_now_ms());

    if (unavailable.isNull()) {
        for (const auto& algorithm : task.algorithms) {
            if (!algorithm.enabled) continue;
            Json::Value model;
            if (models.isArray()) for (const auto& item : models) {
                if (item.isObject() && item["algo_id"] == algorithm.algo_id) { model = item; break; }
            }
            auto readiness = model_readiness(model, "");
            if (readiness["status"] != "available") {
                readiness["algo_id"] = algorithm.algo_id;
                model_issues.append(readiness);
                continue;
            }
            for (const auto& rule : task.rules) {
                if (!rule.enabled || rule.algo_id != algorithm.algo_id) continue;
                if (!supported(rule.capability_id)) {
                    readiness = finding("unsupported", "rule_not_implemented", "当前版本尚未支持此规则。", "更换为已支持的规则。");
                } else if (!supports_type(engine.capabilities["rule_types"], rule.capability_id)) {
                    readiness = finding("blocked", "engine_rule_unsupported", "当前引擎未声明此规则。", "检查引擎版本与规则支持范围。");
                } else {
                    readiness = model_readiness(model, rule.capability_id);
                }
                if (readiness["status"] == "available") continue;
                readiness["algo_id"] = algorithm.algo_id;
                readiness["rule_key"] = rule.key;
                model_issues.append(readiness);
            }
        }
    }

    std::map<int64_t, Json::Value> deployments;
    for (const auto& deployment : view["deployments"])
        deployments.emplace(deployment["camera_id"].asInt64(), deployment);
    unsigned ready = 0;
    Json::Value first_blocked, first_waiting, first_inactive;
    for (const auto camera_id : task.camera_ids) {
        CameraRow camera;
        CameraConfigState config;
        ConfigOperationRow operation;
        const bool known = db_.get_camera(camera_id, camera) && db_.get_camera_config_state(camera_id, config);
        const bool have_operation = known && db_.get_current_config_operation(camera_id, operation);
        const auto deployment = deployments.find(camera_id);
        const bool confirmed = deployment != deployments.end() &&
            deployment->second["task_revision"].asInt64() == task.revision && deployment->second["applied"].asBool();
        const auto revision_key = std::to_string(camera_id);
        const auto engine_revision = version_readback ? revisions[revision_key] : Json::Value();
        const auto frames = frame_states.isObject() ? frame_states[revision_key] : Json::Value();
        const bool matches = engine_revision.isInt64() && config.desired_revision > 0 &&
            engine_revision.asInt64() == config.desired_revision;
        Json::Value row;
        if (!known || config.deleted_at) {
            row = finding("blocked", "camera_missing", "关联摄像头已删除或不可用。", "编辑任务，更新关联摄像头。");
        } else if (!camera.enabled) {
            row = finding("inactive", "camera_disabled", "摄像头已停用。", "到摄像头页面启用此通道。");
        } else if (!task.enabled && deployment == deployments.end()) {
            row = finding("inactive", "task_draft", "任务尚未启用。", "检查配置并启用任务。");
        } else if (!unavailable.isNull()) {
            row = unavailable;
        } else if (config.apply_status == "failed" || config.apply_status == "retry_wait") {
            const bool retry = config.apply_status == "retry_wait";
            row = finding(retry ? "waiting" : "blocked", retry ? "configuration_retry" : "configuration_failed",
                          retry ? "配置执行失败，等待自动重试。" : "配置执行失败。",
                          "查看执行详情，检查摄像头连接、媒体服务及算法包。");
        } else if (!confirmed) {
            row = finding("waiting", "configuration_pending", "当前任务或时间窗的配置尚未确认。", "等待配置执行完成后重新诊断。");
        } else if (!version_readback) {
            row = finding("blocked", "configuration_readback_unavailable", "当前引擎未提供配置版本回读。", "检查引擎版本。");
        } else if (!matches) {
            row = finding("blocked", "configuration_revision_mismatch", "业务目标与引擎回报的配置版本不一致。", "重新诊断。如果仍不一致，查看引擎同步和执行详情。");
        } else if (!task.enabled) {
            row = finding("inactive", "task_disabled", "任务已停用，配置已确认。", "需要检测时启用任务。");
        } else if (!view["schedule_active"].asBool()) {
            row = finding("waiting", "waiting_schedule", "当前不在任务的运行时间窗内。", "等待下个运行时间窗，或调整时间计划。");
        } else if (!has_enabled_models) {
            row = finding("blocked", "no_enabled_models", "任务中没有启用的算法。", "编辑任务并启用至少一个算法。");
        } else if (!model_issues.empty()) {
            row = finding("blocked", "task_model_blocked", "任务所需的模型或规则不可用。", "查看模型与规则诊断并处理阻塞项。");
        } else if (frames == "awaiting_first_frame") {
            row = finding("waiting", "camera_awaiting_frame", "配置已确认，等待摄像头首帧。", "检查摄像头与子码流连接。");
        } else if (frames == "no_recent_frames") {
            row = finding("blocked", "camera_no_recent_frames", "配置已确认，近期没有解码帧。", "检查摄像头与媒体连接。");
        } else if (!camera.online) {
            row = finding("blocked", "camera_offline", "最近一次通道状态为离线。", "检查摄像头及媒体连接。");
        } else {
            row = finding("available", "configuration_confirmed", "引擎配置版本与业务目标一致。");
            ++ready;
        }
        if (row["status"] == "blocked" && first_blocked.isNull()) first_blocked = row;
        if (row["status"] == "waiting" && first_waiting.isNull()) first_waiting = row;
        if (row["status"] == "inactive" && first_inactive.isNull()) first_inactive = row;
        row["camera_id"] = Json::Int64(camera_id);
        row["camera_name"] = camera.name;
        row["desired_revision"] = Json::Int64(config.desired_revision);
        row["applied_revision"] = Json::Int64(config.applied_revision);
        row["engine_revision"] = engine_revision.isInt64() && engine_revision.asInt64() > 0 ? engine_revision : Json::Value();
        if (have_operation) {
            auto& info = row["operation"];
            info["id"] = operation.operation_id;
            info["status"] = operation.status;
            info["attempts"] = operation.attempts;
            info["error_code"] = operation.error_code;
            info["last_error"] = operation.last_error;
            info["next_retry_at"] = Json::Int64(operation.next_retry_at);
        }
        cameras.append(row);
    }
    if (ready && ready == cameras.size())
        data["summary"] = finding("available", "configuration_confirmed", "所有摄像头的任务配置已核对。");
    else if (ready)
        data["summary"] = finding("partial", "partially_ready", "部分摄像头尚未就绪。", "查看各摄像头的诊断结果。");
    else if (!first_blocked.isNull()) data["summary"] = first_blocked;
    else if (!first_waiting.isNull()) data["summary"] = first_waiting;
    else if (!first_inactive.isNull()) data["summary"] = first_inactive;
    else data["summary"] = finding("blocked", "no_cameras", "任务没有关联摄像头。", "为任务选择至少一台摄像头。");
    data["cameras"] = cameras;
    data["model_issues"] = model_issues;
    if (!read.commit()) return fail(5001, "cannot finish reading task diagnostics");
    return {0, "ok", data};
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
