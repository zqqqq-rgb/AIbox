#include "ipc/Protocol.h"

#include <cmath>
#include <json/json.h>
#include <limits>
#include <set>

namespace aibox {
namespace proto {

namespace {

Json::Value make_envelope(const char* kind_key, const char* kind_val) {
    Json::Value root(Json::objectValue);
    root[kind_key] = kind_val;
    return root;
}

std::string to_string(const Json::Value& root) {
    Json::StreamWriterBuilder b;
    b["indentation"] = ""; // JSON-lines：单行紧凑
    b["commentStyle"] = "None";
    return Json::writeString(b, root);
}

std::unique_ptr<Json::CharReader> make_reader() {
    Json::CharReaderBuilder b;
    b["failIfExtra"] = true;
    b["rejectDupKeys"] = true;
    b["allowComments"] = false;
    return std::unique_ptr<Json::CharReader>(b.newCharReader());
}

void put_roi(Json::Value& root, const AlgoCfg& a) {
    if (!a.has_roi) {
        root["roi"] = Json::Value(Json::nullValue); // §8.1：roi:[[x,y]...]|null
        return;
    }
    Json::Value arr(Json::arrayValue);
    for (const auto& pt : a.roi) {
        Json::Value p(Json::arrayValue);
        p.append(pt.first);
        p.append(pt.second);
        arr.append(p);
    }
    root["roi"] = arr;
}

void put_targets(Json::Value& root, const std::vector<DetTarget>& ts) {
    Json::Value arr(Json::arrayValue);
    for (const auto& t : ts) {
        Json::Value o(Json::objectValue);
        o["cls"] = t.cls;
        o["conf"] = t.conf;
        o["x"] = t.x;
        o["y"] = t.y;
        o["w"] = t.w;
        o["h"] = t.h;
        o["track_id"] = t.track_id;
        o["vx"] = t.vx;
        o["vy"] = t.vy;
        arr.append(o);
    }
    root["targets"] = arr;
}

// 逐字段安全取值；任一必填缺失/类型不符返回 kErrProtocol（decode_line 返回 int，
// 宏内 return false 会退化为 return 0=kOk——这里必须显式 kErrProtocol）
#define GET_U32(root, key, dst)                                                                    \
    do {                                                                                           \
        const Json::Value& f = (root)[key];                                                        \
        if (!f.isUInt() || f.asUInt() > 0xFFFFFFFFu)                                               \
            return kErrProtocol;                                                                   \
        (dst) = static_cast<uint32_t>(f.asUInt());                                                 \
    } while (0)
#define GET_U64(root, key, dst)                                                                    \
    do {                                                                                           \
        const Json::Value& f = (root)[key];                                                        \
        if (!f.isUInt64())                                                                         \
            return kErrProtocol;                                                                   \
        (dst) = f.asUInt64();                                                                      \
    } while (0)
#define GET_I64(root, key, dst)                                                                    \
    do {                                                                                           \
        const Json::Value& f = (root)[key];                                                        \
        if (!f.isInt64() && !f.isUInt64())                                                         \
            return kErrProtocol;                                                                   \
        (dst) = f.asInt64();                                                                       \
    } while (0)
#define GET_STR(root, key, dst)                                                                    \
    do {                                                                                           \
        const Json::Value& f = (root)[key];                                                        \
        if (!f.isString())                                                                         \
            return kErrProtocol;                                                                   \
        (dst) = f.asString();                                                                      \
    } while (0)
#define GET_F32(root, key, dst)                                                                    \
    do {                                                                                           \
        const Json::Value& f = (root)[key];                                                        \
        if (!f.isDouble() && !f.isInt())                                                           \
            return kErrProtocol;                                                                   \
        (dst) = static_cast<float>(f.asDouble());                                                  \
    } while (0)

// 返回 kOk / kErrProtocol（与 decode_line 同一返回码域，供宏直接 return）
int parse_target(const Json::Value& o, DetTarget& t) {
    const Json::Value& cls = o["cls"];
    if (!cls.isInt())
        return kErrProtocol;
    t.cls = cls.asInt();
    GET_F32(o, "conf", t.conf);
    GET_F32(o, "x", t.x);
    GET_F32(o, "y", t.y);
    GET_F32(o, "w", t.w);
    GET_F32(o, "h", t.h);
    const Json::Value& tid = o["track_id"];
    if (!tid.isInt())
        return kErrProtocol;
    t.track_id = tid.asInt();
    GET_F32(o, "vx", t.vx);
    GET_F32(o, "vy", t.vy);
    return kOk;
}

int parse_targets(const Json::Value& arr, std::vector<DetTarget>& ts) {
    if (!arr.isArray())
        return kErrProtocol;
    ts.reserve(arr.size());
    for (const auto& o : arr) {
        DetTarget t;
        const int rc = parse_target(o, t);
        if (rc != kOk)
            return rc;
        ts.push_back(t);
    }
    return kOk;
}

} // namespace

// ---- 编码：请求 ----
std::string encode_request(uint64_t msg_id, const AddCam& v) {
    Json::Value r = make_envelope("cmd", "add_cam");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["id"] = v.id;
    r["rtsp_sub"] = v.rtsp_sub;
    if (v.has_rtsp_main)
        r["rtsp_main"] = v.rtsp_main;
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const SetAlgos& v) {
    Json::Value r = make_envelope("cmd", v.config_version == 2 ? "set_rules_v2" : "set_algos");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["id"] = v.id;
    if (v.config_version != 1)
        r["config_version"] = v.config_version;
    Json::Value arr(Json::arrayValue);
    for (const auto& a : v.algos) {
        Json::Value o(Json::objectValue);
        o["algo"] = a.algo;
        o["thr"] = a.thr;
        o["fps"] = a.fps;
        if (v.config_version == 2) {
            o["rules"] = Json::Value(Json::arrayValue);
            for (const auto& rule : a.rules) {
                Json::Value value;
                value["task_id"] = Json::UInt64(rule.task_id);
                value["rule_id"] = Json::UInt64(rule.rule_id);
                value["task_revision"] = Json::UInt64(rule.task_revision);
                value["rule_version"] = Json::UInt64(rule.rule_version);
                value["type"] = rule.type;
                value["event_type"] = rule.event_type;
                value["thr"] = rule.threshold;
                value["fps"] = rule.fps;
                AlgoCfg region;
                region.has_roi = true;
                region.roi = rule.roi;
                put_roi(value, region);
                value["dwell_sec"] = rule.dwell_sec;
                value["debounce_sec"] = rule.debounce_sec;
                o["rules"].append(value);
            }
            arr.append(o);
            continue;
        }
        put_roi(o, a);
        // 规则参数（事件接线）：encode 全量写出（对端按需读取）
        o["rule_type"] = a.rule_type;
        if (!a.event_type.empty())
            o["event_type"] = a.event_type;
        o["dwell_sec"] = a.dwell_sec;
        o["debounce_sec"] = a.debounce_sec;
        arr.append(o);
    }
    r["algos"] = arr;
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const RemoveCam& v) {
    Json::Value r = make_envelope("cmd", "remove_cam");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["id"] = v.id;
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const SubMeta& v) {
    Json::Value r = make_envelope("cmd", v.subscribe ? "sub_meta" : "unsub_meta");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["id"] = v.id;
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const ReloadAlgo& v) {
    Json::Value r = make_envelope("cmd", "reload_algo");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["algo"] = v.algo;
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const GetMetrics&) {
    Json::Value r = make_envelope("cmd", "get_metrics");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const GetCapabilities&) {
    auto root = make_envelope("cmd", "get_capabilities");
    root["msg_id"] = Json::UInt64(msg_id);
    return to_string(root);
}

std::string engine_capabilities_json() {
    Json::Value root;
    root["rule_config_version"] = 2;
    root["rule_command"] = "set_rules_v2";
    root["rule_sources"] = true;
    root["independent_threshold"] = true;
    root["independent_sampling"] = true;
    root["rule_types"] = Json::Value(Json::arrayValue);
    root["rule_types"].append("region_in");
    root["rule_types"].append("dwell");
    root["max_rules_per_camera"] = 256;
    return to_string(root);
}

bool validate_v2_config(const SetAlgos& config, std::string& error) {
    auto bad = [&](const char* text) {
        error = text;
        return false;
    };
    error.clear();
    if (config.config_version != 2 || !config.id || config.algos.size() > 64)
        return bad("invalid V2 configuration header");
    std::set<std::string> models;
    std::set<std::pair<uint64_t, uint64_t>> sources;
    size_t count = 0;
    for (const auto& model : config.algos) {
        if (model.algo.empty() || model.algo.size() > 128 || !models.insert(model.algo).second ||
            !model.has_rules || !std::isfinite(model.thr) || model.thr < 0 || model.thr > 1 ||
            model.fps < 1 || model.fps > 25)
            return bad("invalid or duplicate V2 model");
        for (const auto& rule : model.rules) {
            if (++count > 256)
                return bad("too many rules");
            const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            if (!rule.task_id || !rule.rule_id || !rule.task_revision || !rule.rule_version ||
                rule.task_id > max || rule.rule_id > max || rule.task_revision > max ||
                rule.rule_version > max || !sources.insert({rule.task_id, rule.rule_id}).second)
                return bad("invalid or duplicate rule source");
            if ((rule.type != "region_in" && rule.type != "dwell") ||
                rule.event_type.size() > 128 || !std::isfinite(rule.threshold) ||
                rule.threshold < model.thr || rule.threshold > 1 || rule.fps < 1 ||
                rule.fps > model.fps || (rule.type == "dwell" && !rule.dwell_sec))
                return bad("invalid rule semantics or shared model envelope");
            if (rule.roi.size() < 3 || rule.roi.size() > 128)
                return bad("invalid rule polygon");
            double area = 0;
            for (size_t i = 0; i < rule.roi.size(); ++i) {
                const auto& a = rule.roi[i];
                const auto& b = rule.roi[(i + 1) % rule.roi.size()];
                if (!std::isfinite(a.first) || !std::isfinite(a.second) || a.first < 0 ||
                    a.first > 1 || a.second < 0 || a.second > 1)
                    return bad("invalid polygon coordinate");
                area += a.first * b.second - b.first * a.second;
            }
            if (std::abs(area) < 1e-9)
                return bad("degenerate rule polygon");
        }
    }
    return true;
}

// ---- 编码：事件 ----
std::string encode_event(const CamStatusEvt& v) {
    Json::Value r = make_envelope("evt", "camera_status");
    r["id"] = v.id;
    r["online"] = v.online;
    return to_string(r);
}

std::string encode_event(const RegistryReadyEvt& v) {
    Json::Value r = make_envelope("evt", "registry_ready");
    Json::Value arr(Json::arrayValue);
    for (const auto& a : v.algos) {
        Json::Value o(Json::objectValue);
        o["algo_id"] = a.algo_id;
        o["version"] = a.version;
        o["model_ref"] = a.model_ref;
        o["npu_cost_ms"] = a.npu_cost_ms;
        o["npu_mem_kb"] = a.npu_mem_kb;
        o["trigger"] = a.trigger;
        o["disabled"] = a.disabled;
        if (!a.classes.empty()) {
            Json::Value carr(Json::arrayValue);
            for (const int c : a.classes)
                carr.append(c);
            o["classes"] = carr;
        }
        if (!a.class_names.empty()) {
            Json::Value carr(Json::arrayValue);
            for (const auto& n : a.class_names)
                carr.append(n);
            o["class_names"] = carr;
        }
        arr.append(o);
    }
    r["algos"] = arr;
    return to_string(r);
}

std::string encode_event(const DetectionsEvt& v) {
    Json::Value r = make_envelope("evt", "detections");
    r["id"] = v.id;
    r["ts"] = Json::Value::Int64(v.ts);
    r["frame_seq"] = Json::Value::UInt64(v.frame_seq);
    r["frame_pts"] = Json::Value::Int64(v.frame_pts);
    r["frame_w"] = v.frame_w;
    r["frame_h"] = v.frame_h;
    put_targets(r, v.targets);
    return to_string(r);
}

std::string encode_event(const RuleEventEvt& v) {
    Json::Value r = make_envelope("evt", "event");
    r["id"] = v.id;
    r["algo"] = v.algo;
    r["type"] = v.type;
    r["ts"] = Json::Value::Int64(v.ts);
    r["snap"] = v.snap;
    if (v.task_id || v.rule_id || v.task_revision || v.rule_version) {
        r["task_id"] = Json::UInt64(v.task_id);
        r["rule_id"] = Json::UInt64(v.rule_id);
        r["task_revision"] = Json::UInt64(v.task_revision);
        r["rule_version"] = Json::UInt64(v.rule_version);
    }
    put_targets(r, v.targets);
    return to_string(r);
}

std::string encode_event(const DegradeEvt& v) {
    Json::Value r = make_envelope("evt", "degrade");
    r["level"] = v.level;
    r["reason"] = v.reason;
    return to_string(r);
}

// ---- 编码：ack ----
std::string encode_ack(const Ack& v) {
    Json::Value r(Json::objectValue);
    r["ack"] = Json::Value::UInt64(v.ack);
    r["ret"] = v.ret;
    if (v.ret != kOk) {
        r["msg"] = v.msg;
    } else if (v.has_data) {
        // §8.1: data 为内联 JSON 值（如 get_metrics 响应对象），非字符串
        Json::Value inner;
        Json::String errs;
        if (make_reader()->parse(v.data.data(), v.data.data() + v.data.size(), &inner, &errs)) {
            r["data"] = inner;
        } else {
            r["data"] = v.data; // 防御：非 JSON 片段按字符串透传
        }
    }
    return to_string(r);
}

// ---- 解码 ----
bool supports_rule_v2(const std::string& text) {
    Json::Value value;
    std::string error;
    if (!make_reader()->parse(text.data(), text.data() + text.size(), &value, &error) ||
        !value.isObject())
        return false;
    if (value["rule_config_version"] != 2 || value["rule_command"] != "set_rules_v2")
        return false;
    for (const auto* key : {"rule_sources", "independent_threshold", "independent_sampling"})
        if (!value[key].isBool() || !value[key].asBool())
            return false;
    bool region = false, dwell = false;
    if (!value["rule_types"].isArray())
        return false;
    for (const auto& type : value["rule_types"]) {
        if (type == "region_in")
            region = true;
        if (type == "dwell")
            dwell = true;
    }
    return region && dwell;
}

int decode_line(const std::string& line, ParsedMsg& out) {
    out = ParsedMsg{};

    Json::Value root;
    Json::String errs;
    if (!make_reader()->parse(line.data(), line.data() + line.size(), &root, &errs)) {
        return kErrProtocol;
    }
    if (!root.isObject())
        return kErrProtocol;

    // ack 帧
    if (root.isMember("ack")) {
        Ack a;
        GET_U64(root, "ack", a.ack);
        const Json::Value& ret = root["ret"];
        if (!ret.isInt())
            return kErrProtocol;
        a.ret = ret.asInt();
        if (a.ret != kOk) {
            GET_STR(root, "msg", a.msg);
        } else if (root.isMember("data")) {
            // data 接受任意 JSON 值；对象/数组预序列化存储（API 保持 string 承载）
            const Json::Value& d = root["data"];
            a.data = d.isString() ? d.asString() : to_string(d);
            a.has_data = true;
        }
        out.kind = "ack";
        out.msg_id = a.ack;
        out.ack = a;
        return kOk;
    }

    // 请求帧
    if (root.isMember("cmd")) {
        GET_U64(root, "msg_id", out.msg_id);
        GET_STR(root, "cmd", out.name);
        out.kind = "cmd";
        const std::string& c = out.name;
        if (c == "add_cam") {
            AddCam& v = out.add_cam;
            GET_U32(root, "id", v.id);
            GET_STR(root, "rtsp_sub", v.rtsp_sub);
            if (root.isMember("rtsp_main")) {
                GET_STR(root, "rtsp_main", v.rtsp_main);
                v.has_rtsp_main = true;
            }
        } else if (c == "set_algos" || c == "set_rules_v2") {
            SetAlgos& v = out.set_algos;
            const bool v2 = c == "set_rules_v2";
            if (v2) {
                if (root["config_version"] != 2)
                    return kErrProtocol;
                for (const auto& key : root.getMemberNames())
                    if (key != "cmd" && key != "msg_id" && key != "id" && key != "config_version" &&
                        key != "algos")
                        return kErrProtocol;
                v.config_version = 2;
            } else if (root.isMember("config_version") && root["config_version"] != 1)
                return kErrProtocol;
            GET_U32(root, "id", v.id);
            const Json::Value& arr = root["algos"];
            if (!arr.isArray())
                return kErrProtocol;
            if (v2 && arr.size() > 64)
                return kErrProtocol;
            for (const auto& o : arr) {
                if (!o.isObject())
                    return kErrProtocol;
                AlgoCfg a;
                GET_STR(o, "algo", a.algo);
                GET_F32(o, "thr", a.thr);
                const Json::Value& fps = o["fps"];
                if (!fps.isUInt() || fps.asUInt() > 255)
                    return kErrProtocol;
                a.fps = static_cast<uint8_t>(fps.asUInt());
                if (v2) {
                    for (const auto& key : o.getMemberNames())
                        if (key != "algo" && key != "thr" && key != "fps" && key != "rules")
                            return kErrProtocol;
                    if (!o["rules"].isArray() || o["rules"].size() > 256)
                        return kErrProtocol;
                    a.has_rules = true;
                    for (const auto& value : o["rules"]) {
                        if (!value.isObject())
                            return kErrProtocol;
                        static const std::set<std::string> allowed = {
                            "task_id", "rule_id",    "task_revision", "rule_version",
                            "type",    "event_type", "thr",           "fps",
                            "roi",     "dwell_sec",  "debounce_sec"};
                        for (const auto& key : value.getMemberNames())
                            if (!allowed.count(key))
                                return kErrProtocol;
                        RuleCfg rule;
                        GET_U64(value, "task_id", rule.task_id);
                        GET_U64(value, "rule_id", rule.rule_id);
                        GET_U64(value, "task_revision", rule.task_revision);
                        GET_U64(value, "rule_version", rule.rule_version);
                        GET_STR(value, "type", rule.type);
                        GET_STR(value, "event_type", rule.event_type);
                        GET_F32(value, "thr", rule.threshold);
                        if (!value["fps"].isUInt() || value["fps"].asUInt() > 25)
                            return kErrProtocol;
                        rule.fps = static_cast<uint8_t>(value["fps"].asUInt());
                        GET_U32(value, "dwell_sec", rule.dwell_sec);
                        GET_U32(value, "debounce_sec", rule.debounce_sec);
                        if (!value["roi"].isArray() || value["roi"].size() > 128)
                            return kErrProtocol;
                        for (const auto& point : value["roi"]) {
                            if (!point.isArray() || point.size() != 2)
                                return kErrProtocol;
                            float x, y;
                            GET_F32(point, static_cast<Json::ArrayIndex>(0), x);
                            GET_F32(point, static_cast<Json::ArrayIndex>(1), y);
                            rule.roi.emplace_back(x, y);
                        }
                        a.rules.push_back(std::move(rule));
                    }
                    v.algos.push_back(std::move(a));
                    continue;
                }
                if (o.isMember("rules"))
                    return kErrProtocol;
                const Json::Value& roi = o["roi"];
                if (roi.isArray()) {
                    a.has_roi = true;
                    for (const auto& p : roi) {
                        if (!p.isArray() || p.size() != 2)
                            return kErrProtocol;
                        float x, y;
                        GET_F32(p, static_cast<Json::ArrayIndex>(0), x);
                        GET_F32(p, static_cast<Json::ArrayIndex>(1), y);
                        a.roi.emplace_back(x, y);
                    }
                }
                // 规则参数可选（老版 businessd 无此字段：缺省用 AlgoCfg 默认值，
                // 向后兼容——与 classes/class_names 同一容错策略）
                const Json::Value& rt = o["rule_type"];
                if (rt.isString())
                    a.rule_type = rt.asString();
                const Json::Value& et = o["event_type"];
                if (et.isString())
                    a.event_type = et.asString();
                const Json::Value& dw = o["dwell_sec"];
                if (dw.isUInt())
                    a.dwell_sec = dw.asUInt();
                const Json::Value& db = o["debounce_sec"];
                if (db.isUInt())
                    a.debounce_sec = db.asUInt();
                v.algos.push_back(std::move(a));
            }
            if (v2) {
                std::string error;
                if (!validate_v2_config(v, error))
                    return kErrProtocol;
            }
        } else if (c == "remove_cam") {
            GET_U32(root, "id", out.remove_cam.id);
        } else if (c == "sub_meta" || c == "unsub_meta") {
            out.sub_meta.subscribe = (c == "sub_meta");
            GET_U32(root, "id", out.sub_meta.id);
        } else if (c == "reload_algo") {
            GET_STR(root, "algo", out.reload_algo.algo);
        } else if (c == "get_metrics") {
            ; // 无字段
        } else if (c == "get_capabilities") {
            ;
        } else {
            return kErrProtocol; // 未知 cmd
        }
        return kOk;
    }

    // 事件帧
    if (root.isMember("evt")) {
        GET_STR(root, "evt", out.name);
        out.kind = "evt";
        const std::string& e = out.name;
        if (e == "camera_status") {
            CamStatusEvt& v = out.camera_status;
            GET_U32(root, "id", v.id);
            const Json::Value& on = root["online"];
            if (!on.isBool())
                return kErrProtocol;
            v.online = on.asBool();
        } else if (e == "registry_ready") {
            const Json::Value& arr = root["algos"];
            if (!arr.isArray())
                return kErrProtocol;
            for (const auto& o : arr) {
                RegistryAlgo a;
                GET_STR(o, "algo_id", a.algo_id);
                GET_STR(o, "version", a.version);
                GET_STR(o, "model_ref", a.model_ref);
                GET_F32(o, "npu_cost_ms", a.npu_cost_ms);
                GET_U32(o, "npu_mem_kb", a.npu_mem_kb);
                GET_STR(o, "trigger", a.trigger);
                const Json::Value& dis = o["disabled"];
                if (!dis.isBool())
                    return kErrProtocol;
                a.disabled = dis.asBool();
                // classes 可选（老版 engine 无此字段：缺省空 = 全类，向后兼容）
                const Json::Value& carr = o["classes"];
                if (carr.isArray()) {
                    for (const Json::Value& c : carr) {
                        if (!c.isInt())
                            return kErrProtocol;
                        a.classes.push_back(c.asInt());
                    }
                }
                // class_names 可选（同上向后兼容；下标即 id）
                const Json::Value& narr = o["class_names"];
                if (narr.isArray()) {
                    for (const Json::Value& n : narr) {
                        if (!n.isString())
                            return kErrProtocol;
                        a.class_names.push_back(n.asString());
                    }
                }
                out.registry_ready.algos.push_back(std::move(a));
            }
        } else if (e == "detections") {
            DetectionsEvt& v = out.detections;
            GET_U32(root, "id", v.id);
            GET_I64(root, "ts", v.ts);
            GET_U64(root, "frame_seq", v.frame_seq);
            GET_I64(root, "frame_pts", v.frame_pts);
            GET_U32(root, "frame_w", v.frame_w);
            GET_U32(root, "frame_h", v.frame_h);
            if (parse_targets(root["targets"], v.targets) != kOk)
                return kErrProtocol;
        } else if (e == "event") {
            RuleEventEvt& v = out.event;
            GET_U32(root, "id", v.id);
            GET_STR(root, "algo", v.algo);
            GET_STR(root, "type", v.type);
            GET_I64(root, "ts", v.ts);
            GET_STR(root, "snap", v.snap);
            if (root.isMember("task_id") || root.isMember("rule_id") ||
                root.isMember("task_revision") || root.isMember("rule_version")) {
                GET_U64(root, "task_id", v.task_id);
                GET_U64(root, "rule_id", v.rule_id);
                GET_U64(root, "task_revision", v.task_revision);
                GET_U64(root, "rule_version", v.rule_version);
                const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                if (!v.task_id || !v.rule_id || !v.task_revision || !v.rule_version ||
                    v.task_id > max || v.rule_id > max || v.task_revision > max ||
                    v.rule_version > max)
                    return kErrProtocol;
            }
            if (parse_targets(root["targets"], v.targets) != kOk)
                return kErrProtocol;
        } else if (e == "degrade") {
            DegradeEvt& v = out.degrade;
            const Json::Value& lv = root["level"];
            if (!lv.isUInt() || lv.asUInt() > 255)
                return kErrProtocol;
            v.level = static_cast<uint8_t>(lv.asUInt());
            GET_STR(root, "reason", v.reason);
        } else {
            return kErrProtocol; // 未知 evt
        }
        return kOk;
    }

    return kErrProtocol; // 无 cmd/evt/ack 任一键
}

#undef GET_U32
#undef GET_U64
#undef GET_I64
#undef GET_STR
#undef GET_F32

} // namespace proto
} // namespace aibox
