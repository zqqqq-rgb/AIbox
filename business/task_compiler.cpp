#include "task_compiler.h"
#include "config_compiler.h"
#include <algorithm>
#include <chrono>
#include <memory>
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
int offset(const std::string& zone) {
    if (zone == "UTC")
        return 0;
    if (zone.size() != 9 || zone.substr(0, 3) != "UTC" || (zone[3] != '+' && zone[3] != '-') ||
        zone[6] != ':')
        return 10000;
    for (auto i : {4, 5, 7, 8})
        if (zone[i] < '0' || zone[i] > '9')
            return 10000;
    int hour = (zone[4] - '0') * 10 + zone[5] - '0', minute = (zone[7] - '0') * 10 + zone[8] - '0';
    if (hour > 14 || minute > 59 || (hour == 14 && minute))
        return 10000;
    return (zone[3] == '+' ? 1 : -1) * (hour * 60 + minute);
}
int64_t floor_div(int64_t value, int64_t divisor) {
    auto q = value / divisor;
    return q - (value % divisor < 0);
}
} // namespace
bool valid_task_schedule(const TaskSchedule& s) {
    return offset(s.timezone) != 10000 && s.weekdays > 0 && s.weekdays <= 127 &&
           s.start_minute >= 0 && s.start_minute < 1440 && s.end_minute >= 0 &&
           s.end_minute <= 1440 && s.start_minute != s.end_minute;
}
bool schedule_matches(const TaskSchedule& s, int64_t at_ms) {
    if (!valid_task_schedule(s))
        return false;
    const auto local = floor_div(at_ms, 60000) + offset(s.timezone), day = floor_div(local, 1440);
    const int minute = static_cast<int>(local - day * 1440),
              weekday = static_cast<int>(((day + 3) % 7 + 7) % 7);
    if (s.start_minute < s.end_minute)
        return (s.weekdays & (1 << weekday)) && minute >= s.start_minute && minute < s.end_minute;
    return (minute >= s.start_minute && (s.weekdays & (1 << weekday))) ||
           (minute < s.end_minute && (s.weekdays & (1 << ((weekday + 6) % 7))));
}
int64_t task_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
bool task_window_matches(const AlgorithmTask& task, int64_t at_ms) {
    return task.schedules.empty() || std::any_of(task.schedules.begin(), task.schedules.end(),
                                                 [&](const TaskSchedule& schedule) {
                                                     return schedule_matches(schedule, at_ms);
                                                 });
}
CompiledTaskPlan compile_task_plan(const std::vector<AlgorithmTask>& tasks, int64_t id, int64_t at,
                                   bool include_drafts) {
    std::map<std::string, Json::Value> models;
    std::vector<std::string> errors;
    Json::Value excluded(Json::arrayValue);
    for (const auto& task : tasks) {
        if (std::find(task.camera_ids.begin(), task.camera_ids.end(), id) == task.camera_ids.end())
            continue;
        if (!task.enabled && !(include_drafts && task.kind == "v2"))
            continue;
        if (std::any_of(
                task.schedules.begin(), task.schedules.end(),
                [](const TaskSchedule& schedule) { return !valid_task_schedule(schedule); })) {
            errors.push_back("invalid stored schedule for task " + std::to_string(task.id));
            continue;
        }
        if (!task.schedules.empty() &&
            !std::any_of(task.schedules.begin(), task.schedules.end(),
                         [&](const TaskSchedule& s) { return schedule_matches(s, at); })) {
            excluded.append(static_cast<Json::Int64>(task.id));
            continue;
        }
        for (const auto& a : task.algorithms)
            if (a.enabled) {
                CamAlgoRow check;
                check.algo_id = a.algo_id;
                check.threshold = a.threshold;
                check.detect_fps = a.fps;
                proto::SetAlgos checked;
                std::string validation_error;
                if (!compile_algos(static_cast<uint32_t>(id), {check}, checked, validation_error)) {
                    errors.push_back("task " + std::to_string(task.id) + ": " + validation_error);
                    continue;
                }
                auto& model = models[a.algo_id];
                if (model.isNull()) {
                    model["algo_id"] = a.algo_id;
                    model["fps"] = a.fps;
                    model["threshold"] = a.threshold;
                    model["sources"] = Json::Value(Json::arrayValue);
                    model["rules"] = Json::Value(Json::arrayValue);
                }
                model["fps"] = std::max(model["fps"].asInt(), a.fps);
                model["threshold"] = std::min(model["threshold"].asDouble(), a.threshold);
                Json::Value source;
                source["task_id"] = static_cast<Json::Int64>(task.id);
                source["task_revision"] = static_cast<Json::Int64>(task.revision);
                source["threshold"] = a.threshold;
                source["fps"] = a.fps;
                model["sources"].append(source);
                for (const auto& r : task.rules)
                    if (r.algo_id == a.algo_id && r.enabled) {
                        if (task.kind == "legacy" && r.roi_json.empty())
                            continue; // Legacy detector-only rows have no alarm region.
                        auto params = r.params_json.empty() ? Json::Value(Json::objectValue)
                                                            : parse(r.params_json);
                        if (!params.isObject()) {
                            errors.push_back("invalid stored rule " + std::to_string(r.id));
                            continue;
                        }
                        if ((params.isMember("rule_type") && !params["rule_type"].isString()) ||
                            (params.isMember("debounce_sec") && !params["debounce_sec"].isUInt())) {
                            errors.push_back("invalid stored rule parameters " +
                                             std::to_string(r.id));
                            continue;
                        }
                        const auto type = task.kind == "legacy"
                                              ? params.get("rule_type", "region_in").asString()
                                              : r.capability_id;
                        params["rule_type"] = type;
                        if (task.kind != "legacy" && params.get("debounce_sec", 0).asUInt() == 0)
                            params["debounce_sec"] = 30;
                        CamAlgoRow row;
                        row.algo_id = a.algo_id;
                        row.threshold = a.threshold;
                        row.detect_fps = a.fps;
                        row.roi_json = r.roi_json;
                        row.rule_json = dump(params);
                        proto::SetAlgos compiled;
                        std::string error;
                        if (!compile_algos(static_cast<uint32_t>(id), {row}, compiled, error)) {
                            errors.push_back("rule " + std::to_string(r.id) + ": " + error);
                            continue;
                        }
                        auto rule = source;
                        rule["rule_id"] = static_cast<Json::Int64>(r.id);
                        rule["rule_version"] = static_cast<Json::Int64>(r.version);
                        rule["type"] = type;
                        rule["roi"] = parse(r.roi_json);
                        rule["event_type"] = compiled.algos[0].event_type.empty()
                                                 ? a.algo_id + "_" + type
                                                 : compiled.algos[0].event_type;
                        rule["dwell_sec"] = compiled.algos[0].dwell_sec;
                        rule["debounce_sec"] = compiled.algos[0].debounce_sec;
                        model["rules"].append(rule);
                    }
            }
    }
    return {std::move(models), std::move(errors), std::move(excluded)};
}
bool compile_task_wire(const CompiledTaskPlan& plan, uint32_t camera, proto::SetAlgos& output,
                       std::string& error) {
    output = {};
    if (!plan.errors.empty()) {
        error = plan.errors.front();
        return false;
    }
    Json::Value root;
    root["cmd"] = "set_rules_v2";
    root["msg_id"] = 1;
    root["config_version"] = 2;
    root["id"] = camera;
    root["algos"] = Json::Value(Json::arrayValue);
    for (const auto& entry : plan.models) {
        const auto& model = entry.second;
        Json::Value value;
        value["algo"] = entry.first;
        value["thr"] = model["threshold"];
        value["fps"] = model["fps"];
        value["rules"] = Json::Value(Json::arrayValue);
        for (auto rule : model["rules"]) {
            rule["thr"] = rule["threshold"];
            rule.removeMember("threshold");
            value["rules"].append(rule);
        }
        root["algos"].append(value);
    }
    proto::ParsedMsg parsed;
    if (proto::decode_line(dump(root), parsed) != proto::kOk) {
        error = "invalid V2 rule plan";
        return false;
    }
    output = std::move(parsed.set_algos);
    error.clear();
    return true;
}
} // namespace biz
} // namespace aibox
