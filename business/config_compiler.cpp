#include "config_compiler.h"
#include "task_compiler.h"

#include <cmath>
#include <algorithm>
#include <memory>
#include <set>

#include <json/json.h>

namespace aibox {
namespace biz {
namespace {
bool parse(const std::string& text, Json::Value& value) {
    Json::CharReaderBuilder builder;
    builder["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string error;
    return reader->parse(text.data(), text.data() + text.size(), &value, &error);
}
}  // namespace

bool compile_algos(uint32_t camera_id, const std::vector<CamAlgoRow>& rows,
                   proto::SetAlgos& out, std::string& error) {
    out = {};
    out.id = camera_id;
    error.clear();
    proto::SetAlgos compiled;
    compiled.id = camera_id;
    std::set<std::string> ids;
    for (const auto& row : rows) {
        auto fail = [&](const char* reason) {
            error = row.algo_id + ": " + reason;
            return false;
        };
        if (row.algo_id.empty() || !ids.insert(row.algo_id).second)
            return fail("empty or duplicate algorithm id");
        if (!row.enabled) continue;
        if (!std::isfinite(row.threshold) || row.threshold < 0 || row.threshold > 1)
            return fail("threshold must be finite and in [0,1]");
        if (row.detect_fps < 1 || row.detect_fps > 25)
            return fail("fps must be in [1,25]");
        proto::AlgoCfg cfg;
        cfg.algo = row.algo_id;
        cfg.thr = static_cast<float>(row.threshold);
        cfg.fps = static_cast<uint8_t>(row.detect_fps);
        if (!row.roi_json.empty()) {
            Json::Value roi;
            if (!parse(row.roi_json, roi) || !roi.isArray() || roi.size() < 3)
                return fail("ROI must contain at least three normalized points");
            for (const auto& point : roi) {
                if (!point.isArray() || point.size() != 2 ||
                    !point[0].isNumeric() || !point[1].isNumeric())
                    return fail("invalid ROI point");
                const double x = point[0].asDouble(), y = point[1].asDouble();
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 ||
                    y < 0 || y > 1) return fail("ROI point outside [0,1]");
                cfg.roi.emplace_back(static_cast<float>(x), static_cast<float>(y));
            }
            double area = 0;
            for (size_t i = 0; i < cfg.roi.size(); ++i) {
                const auto& a = cfg.roi[i];
                const auto& b = cfg.roi[(i + 1) % cfg.roi.size()];
                area += a.first * b.second - b.first * a.second;
            }
            if (std::abs(area) < 1e-9) return fail("ROI has zero area");
            cfg.has_roi = true;
        }
        if (!row.rule_json.empty()) {
            Json::Value rule;
            if (!parse(row.rule_json, rule) || !rule.isObject())
                return fail("rule must be a JSON object");
            if (rule.isMember("rule_type")) {
                if (!rule["rule_type"].isString()) return fail("invalid rule_type");
                cfg.rule_type = rule["rule_type"].asString();
            }
            if (cfg.rule_type != "region_in" && cfg.rule_type != "dwell")
                return fail("unsupported rule_type");
            if (rule.isMember("event_type")) {
                if (!rule["event_type"].isString()) return fail("invalid event_type");
                cfg.event_type = rule["event_type"].asString();
            }
            if (rule.isMember("dwell_sec")) {
                if (!rule["dwell_sec"].isUInt() || rule["dwell_sec"].asUInt() == 0)
                    return fail("dwell_sec must be positive");
                cfg.dwell_sec = rule["dwell_sec"].asUInt();
            }
            if (rule.isMember("debounce_sec")) {
                if (!rule["debounce_sec"].isUInt()) return fail("invalid debounce_sec");
                cfg.debounce_sec = rule["debounce_sec"].asUInt();
            }
        }
        compiled.algos.push_back(std::move(cfg));
    }
    out = std::move(compiled);
    return true;
}
std::string encode_camera_config(const CameraRow& cam, const std::vector<CamAlgoRow>& algos,
                                  bool deleted) {
    Json::Value root(Json::objectValue);
    root["version"] = 1;
    root["id"] = static_cast<Json::Int64>(cam.id);
    root["name"] = cam.name;
    root["rtsp_main"] = cam.rtsp_main;
    root["rtsp_sub"] = cam.rtsp_sub;
    root["enabled"] = cam.enabled;
    root["deleted"] = deleted;
    root["algos"] = Json::Value(Json::arrayValue);
    for (const auto& row : algos) {
        Json::Value a;
        a["algo_id"] = row.algo_id;
        a["threshold"] = row.threshold;
        a["fps"] = row.detect_fps;
        a["enabled"] = row.enabled;
        a["roi_json"] = row.roi_json;
        a["rule_json"] = row.rule_json;
        a["roi_display"] = row.roi_display;
        a["roi_color"] = row.roi_color;
        root["algos"].append(a);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, root);
}

bool decode_camera_config(const std::string& text, CameraConfigSnapshot& out) {
    Json::Value root;
    if (!parse(text, root) || !root.isObject() || (root["version"] != 1 && root["version"] != 2) ||
        !root["id"].isInt64() || !root["name"].isString() ||
        !root["rtsp_main"].isString() || !root["rtsp_sub"].isString() ||
        !root["enabled"].isBool() || !root["deleted"].isBool() || !root["algos"].isArray()) return false;
    CameraConfigSnapshot value;
    value.camera.id = root["id"].asInt64();
    value.camera.name = root["name"].asString();
    value.camera.rtsp_main = root["rtsp_main"].asString();
    value.camera.rtsp_sub = root["rtsp_sub"].asString();
    value.camera.enabled = root["enabled"].asBool();
    value.deleted = root["deleted"].asBool();
    if (root["version"] == 2) {
        Json::StreamWriterBuilder writer; writer["indentation"]="";
        proto::ParsedMsg parsed;
        if (!root["runtime"].isObject() ||
            proto::decode_line(Json::writeString(writer,root["runtime"]),parsed)!=proto::kOk ||
            parsed.name!="set_rules_v2" || parsed.set_algos.id!=value.camera.id) return false;
        value.has_v2=true; value.runtime=std::move(parsed.set_algos);
    }
    for (const auto& a : root["algos"]) {
        if (!a.isObject() || !a["algo_id"].isString() || !a["threshold"].isNumeric() ||
            !a["fps"].isInt() || !a["enabled"].isBool() || !a["roi_json"].isString() ||
            !a["rule_json"].isString() || !a["roi_display"].isInt() || !a["roi_color"].isString()) return false;
        CamAlgoRow row;
        row.algo_id = a["algo_id"].asString(); row.threshold = a["threshold"].asDouble();
        row.detect_fps = a["fps"].asInt(); row.enabled = a["enabled"].asBool();
        row.roi_json = a["roi_json"].asString(); row.rule_json = a["rule_json"].asString();
        row.roi_display = a["roi_display"].asInt(); row.roi_color = a["roi_color"].asString();
        value.algos.push_back(std::move(row));
    }
    out = std::move(value);
    return true;
}

std::string encode_camera_config(const CameraConfigSnapshot& target) {
    if (!target.has_v2) return encode_camera_config(target.camera,target.algos,target.deleted);
    Json::Value root, runtime;
    if (!parse(encode_camera_config(target.camera,target.algos,target.deleted),root) ||
        !parse(proto::encode_request(1,target.runtime),runtime)) return "";
    root["version"]=2; root["runtime"]=runtime;
    Json::StreamWriterBuilder writer; writer["indentation"]="";
    return Json::writeString(writer,root);
}

bool build_camera_config(Db& db, const CameraRow& camera, const std::vector<CamAlgoRow>& rows,
                         bool deleted, CameraConfigSnapshot& out, std::string& error,
                         const std::vector<AlgorithmTask>* supplied, int64_t at_ms) {
    out={camera,rows,deleted}; error.clear();
    auto tasks=supplied?*supplied:db.list_tasks();
    bool active=false;
    for (const auto& task:tasks)
        if (task.kind=="v2" && task.enabled && !task.deleted_at &&
            std::find(task.camera_ids.begin(),task.camera_ids.end(),camera.id)!=task.camera_ids.end()) {
            active=true;
        }
    if (!active || deleted || !camera.enabled) return true;
    // Candidate legacy edits are validated before persistence. Actual snapshots are rebuilt
    // after the legacy setter assigns stable database rule IDs in the same transaction.
    for (auto& task:tasks) if (task.kind=="legacy" && task.legacy_camera_id==camera.id) {
        auto previous=task.rules; task.rules.clear(); task.algorithms.clear();
        size_t index=0;
        for (const auto& row:rows) {
            task.algorithms.push_back({row.algo_id,row.threshold,row.detect_fps,row.enabled,row.roi_display,row.roi_color});
            TaskRule rule; rule.algo_id=row.algo_id; rule.capability_id="region_in";
            rule.id=INT64_MAX-static_cast<int64_t>(index++); rule.roi_json=row.roi_json;
            rule.params_json=row.rule_json; rule.enabled=row.enabled;
            for (const auto& old:previous) if(old.algo_id==row.algo_id) { rule.id=old.id; rule.version=old.version; break; }
            task.rules.push_back(std::move(rule));
        }
    }
    auto plan=compile_task_plan(tasks,camera.id,at_ms < 0 ? task_now_ms() : at_ms,false);
    if (!compile_task_wire(plan,static_cast<uint32_t>(camera.id),out.runtime,error)) return false;
    out.has_v2=true; return true;
}

bool compile_camera_config(const CameraConfigSnapshot& target, const std::string& media_host,
                            int media_port, CompiledCameraConfig& out, std::string& error) {
    out = {};
    error.clear();
    const auto& cam = target.camera;
    auto rtsp = [](const std::string& url) {
        return url.compare(0, 7, "rtsp://") == 0 || url.compare(0, 8, "rtsps://") == 0;
    };
    if (cam.id < 0 || cam.id > UINT32_MAX || cam.name.empty() || media_host.empty() ||
        media_port < 1 || media_port > 65535 ||
        (cam.enabled && !target.deleted && (!rtsp(cam.rtsp_main) || !rtsp(cam.rtsp_sub)))) {
        error = "invalid camera or media endpoint";
        return false;
    }
    out.add.id = out.algos.id = static_cast<uint32_t>(cam.id);
    if (!cam.enabled || target.deleted) return true;
    if (!compile_algos(out.add.id, target.algos, out.algos, error)) return false;
    if (target.has_v2) {
        if (target.runtime.id!=cam.id || !proto::validate_v2_config(target.runtime,error)) return false;
        out.algos=target.runtime;
    }
    const auto prefix = "rtsp://" + media_host + ":" + std::to_string(media_port) +
                        "/live/cam" + std::to_string(cam.id);
    out.add.rtsp_sub = prefix + "_sub";
    out.add.rtsp_main = prefix + "_main";
    out.add.has_rtsp_main = true;
    return true;
}
}  // namespace biz
}  // namespace aibox
