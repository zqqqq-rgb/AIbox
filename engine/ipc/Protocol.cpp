#include "ipc/Protocol.h"

#include <json/json.h>

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
    b["indentation"] = "";  // JSON-lines：单行紧凑
    b["commentStyle"] = "None";
    return Json::writeString(b, root);
}

std::unique_ptr<Json::CharReader> make_reader() {
    Json::CharReaderBuilder b;
    return std::unique_ptr<Json::CharReader>(b.newCharReader());
}

void put_roi(Json::Value& root, const AlgoCfg& a) {
    if (!a.has_roi) {
        root["roi"] = Json::Value(Json::nullValue);  // §8.1：roi:[[x,y]...]|null
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
#define GET_U32(root, key, dst)                                            \
    do {                                                                   \
        const Json::Value& f = (root)[key];                                \
        if (!f.isUInt() || f.asUInt() > 0xFFFFFFFFu) return kErrProtocol;  \
        (dst) = static_cast<uint32_t>(f.asUInt());                         \
    } while (0)
#define GET_U64(root, key, dst)                 \
    do {                                        \
        const Json::Value& f = (root)[key];     \
        if (!f.isUInt64()) return kErrProtocol; \
        (dst) = f.asUInt64();                   \
    } while (0)
#define GET_I64(root, key, dst)                               \
    do {                                                      \
        const Json::Value& f = (root)[key];                   \
        if (!f.isInt64() && !f.isUInt64()) return kErrProtocol; \
        (dst) = f.asInt64();                                  \
    } while (0)
#define GET_STR(root, key, dst)                 \
    do {                                        \
        const Json::Value& f = (root)[key];     \
        if (!f.isString()) return kErrProtocol; \
        (dst) = f.asString();                   \
    } while (0)
#define GET_F32(root, key, dst)                               \
    do {                                                      \
        const Json::Value& f = (root)[key];                   \
        if (!f.isDouble() && !f.isInt()) return kErrProtocol; \
        (dst) = static_cast<float>(f.asDouble());             \
    } while (0)

// 返回 kOk / kErrProtocol（与 decode_line 同一返回码域，供宏直接 return）
int parse_target(const Json::Value& o, DetTarget& t) {
    const Json::Value& cls = o["cls"];
    if (!cls.isInt()) return kErrProtocol;
    t.cls = cls.asInt();
    GET_F32(o, "conf", t.conf);
    GET_F32(o, "x", t.x);
    GET_F32(o, "y", t.y);
    GET_F32(o, "w", t.w);
    GET_F32(o, "h", t.h);
    const Json::Value& tid = o["track_id"];
    if (!tid.isInt()) return kErrProtocol;
    t.track_id = tid.asInt();
    GET_F32(o, "vx", t.vx);
    GET_F32(o, "vy", t.vy);
    return kOk;
}

int parse_targets(const Json::Value& arr, std::vector<DetTarget>& ts) {
    if (!arr.isArray()) return kErrProtocol;
    ts.reserve(arr.size());
    for (const auto& o : arr) {
        DetTarget t;
        const int rc = parse_target(o, t);
        if (rc != kOk) return rc;
        ts.push_back(t);
    }
    return kOk;
}

}  // namespace

// ---- 编码：请求 ----
std::string encode_request(uint64_t msg_id, const AddCam& v) {
    Json::Value r = make_envelope("cmd", "add_cam");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["id"] = v.id;
    r["rtsp_sub"] = v.rtsp_sub;
    if (v.has_rtsp_main) r["rtsp_main"] = v.rtsp_main;
    return to_string(r);
}

std::string encode_request(uint64_t msg_id, const SetAlgos& v) {
    Json::Value r = make_envelope("cmd", "set_algos");
    r["msg_id"] = Json::Value::UInt64(msg_id);
    r["id"] = v.id;
    Json::Value arr(Json::arrayValue);
    for (const auto& a : v.algos) {
        Json::Value o(Json::objectValue);
        o["algo"] = a.algo;
        o["thr"] = a.thr;
        o["fps"] = a.fps;
        put_roi(o, a);
        // 规则参数（事件接线）：encode 全量写出（对端按需读取）
        o["rule_type"] = a.rule_type;
        if (!a.event_type.empty()) o["event_type"] = a.event_type;
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
    Json::Value r =
        make_envelope("cmd", v.subscribe ? "sub_meta" : "unsub_meta");
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
            for (const int c : a.classes) carr.append(c);
            o["classes"] = carr;
        }
        if (!a.class_names.empty()) {
            Json::Value carr(Json::arrayValue);
            for (const auto& n : a.class_names) carr.append(n);
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
            r["data"] = v.data;  // 防御：非 JSON 片段按字符串透传
        }
    }
    return to_string(r);
}

// ---- 解码 ----
int decode_line(const std::string& line, ParsedMsg& out) {
    out = ParsedMsg{};

    Json::Value root;
    Json::String errs;
    if (!make_reader()->parse(line.data(), line.data() + line.size(), &root, &errs)) {
        return kErrProtocol;
    }
    if (!root.isObject()) return kErrProtocol;

    // ack 帧
    if (root.isMember("ack")) {
        Ack a;
        GET_U64(root, "ack", a.ack);
        const Json::Value& ret = root["ret"];
        if (!ret.isInt()) return kErrProtocol;
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
        } else if (c == "set_algos") {
            SetAlgos& v = out.set_algos;
            GET_U32(root, "id", v.id);
            const Json::Value& arr = root["algos"];
            if (!arr.isArray()) return kErrProtocol;
            for (const auto& o : arr) {
                AlgoCfg a;
                GET_STR(o, "algo", a.algo);
                GET_F32(o, "thr", a.thr);
                const Json::Value& fps = o["fps"];
                if (!fps.isUInt() || fps.asUInt() > 255) return kErrProtocol;
                a.fps = static_cast<uint8_t>(fps.asUInt());
                const Json::Value& roi = o["roi"];
                if (roi.isArray()) {
                    a.has_roi = true;
                    for (const auto& p : roi) {
                        if (!p.isArray() || p.size() != 2) return kErrProtocol;
                        float x, y;
                        GET_F32(p, static_cast<Json::ArrayIndex>(0), x);
                        GET_F32(p, static_cast<Json::ArrayIndex>(1), y);
                        a.roi.emplace_back(x, y);
                    }
                }
                // 规则参数可选（老版 businessd 无此字段：缺省用 AlgoCfg 默认值，
                // 向后兼容——与 classes/class_names 同一容错策略）
                const Json::Value& rt = o["rule_type"];
                if (rt.isString()) a.rule_type = rt.asString();
                const Json::Value& et = o["event_type"];
                if (et.isString()) a.event_type = et.asString();
                const Json::Value& dw = o["dwell_sec"];
                if (dw.isUInt()) a.dwell_sec = dw.asUInt();
                const Json::Value& db = o["debounce_sec"];
                if (db.isUInt()) a.debounce_sec = db.asUInt();
                v.algos.push_back(std::move(a));
            }
        } else if (c == "remove_cam") {
            GET_U32(root, "id", out.remove_cam.id);
        } else if (c == "sub_meta" || c == "unsub_meta") {
            out.sub_meta.subscribe = (c == "sub_meta");
            GET_U32(root, "id", out.sub_meta.id);
        } else if (c == "reload_algo") {
            GET_STR(root, "algo", out.reload_algo.algo);
        } else if (c == "get_metrics") {
            ;  // 无字段
        } else {
            return kErrProtocol;  // 未知 cmd
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
            if (!on.isBool()) return kErrProtocol;
            v.online = on.asBool();
        } else if (e == "registry_ready") {
            const Json::Value& arr = root["algos"];
            if (!arr.isArray()) return kErrProtocol;
            for (const auto& o : arr) {
                RegistryAlgo a;
                GET_STR(o, "algo_id", a.algo_id);
                GET_STR(o, "version", a.version);
                GET_STR(o, "model_ref", a.model_ref);
                GET_F32(o, "npu_cost_ms", a.npu_cost_ms);
                GET_U32(o, "npu_mem_kb", a.npu_mem_kb);
                GET_STR(o, "trigger", a.trigger);
                const Json::Value& dis = o["disabled"];
                if (!dis.isBool()) return kErrProtocol;
                a.disabled = dis.asBool();
                // classes 可选（老版 engine 无此字段：缺省空 = 全类，向后兼容）
                const Json::Value& carr = o["classes"];
                if (carr.isArray()) {
                    for (const Json::Value& c : carr) {
                        if (!c.isInt()) return kErrProtocol;
                        a.classes.push_back(c.asInt());
                    }
                }
                // class_names 可选（同上向后兼容；下标即 id）
                const Json::Value& narr = o["class_names"];
                if (narr.isArray()) {
                    for (const Json::Value& n : narr) {
                        if (!n.isString()) return kErrProtocol;
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
            if (parse_targets(root["targets"], v.targets) != kOk) return kErrProtocol;
        } else if (e == "event") {
            RuleEventEvt& v = out.event;
            GET_U32(root, "id", v.id);
            GET_STR(root, "algo", v.algo);
            GET_STR(root, "type", v.type);
            GET_I64(root, "ts", v.ts);
            GET_STR(root, "snap", v.snap);
            if (parse_targets(root["targets"], v.targets) != kOk) return kErrProtocol;
        } else if (e == "degrade") {
            DegradeEvt& v = out.degrade;
            const Json::Value& lv = root["level"];
            if (!lv.isUInt() || lv.asUInt() > 255) return kErrProtocol;
            v.level = static_cast<uint8_t>(lv.asUInt());
            GET_STR(root, "reason", v.reason);
        } else {
            return kErrProtocol;  // 未知 evt
        }
        return kOk;
    }

    return kErrProtocol;  // 无 cmd/evt/ack 任一键
}

#undef GET_U32
#undef GET_U64
#undef GET_I64
#undef GET_STR
#undef GET_F32

}  // namespace proto
}  // namespace aibox
