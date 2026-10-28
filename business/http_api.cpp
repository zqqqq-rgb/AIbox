#include "http_api.h"

#include <httplib.h>

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/Log.h"
#include "config_compiler.h"
#include "http_compat.h"
#include "onvif_client.h"
#include "uds_client.h"
#include "zlm_client.h"

namespace aibox {
namespace biz {

namespace {

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string dump(const Json::Value& v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["commentStyle"] = "None";
    return Json::writeString(b, v);
}

bool parse_body(const std::string& body, Json::Value* out) {
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    std::string errs;
    return rd->parse(body.data(), body.data() + body.size(), out, &errs);
}

// 解析 DB 内 JSON 文本片段（roi/rule 落库形态）为 Json::Value；空串/坏 JSON → false
bool parse_db_json(const std::string& s, Json::Value* out) {
    if (s.empty()) return false;
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    std::string errs;
    return rd->parse(s.data(), s.data() + s.size(), out, &errs);
}

Json::Value envelope(int code, const std::string& msg, const Json::Value* data) {
    Json::Value root(Json::objectValue);
    root["code"] = code;
    root["msg"] = msg;
    if (data) root["data"] = *data;
    return root;
}

void reply(httplib::Response& res, int code, const std::string& msg,
           const Json::Value* data = nullptr) {
    Json::Value env = envelope(code, msg, data);
    res.set_content(dump(env), "application/json");
}

void reply_data(httplib::Response& res, const Json::Value& data) {
    Json::Value env = envelope(0, "ok", &data);
    res.set_content(dump(env), "application/json");
}

void reply_err(httplib::Response& res, int code, const std::string& msg) {
    LOGW("http-api", "code=%d %s", code, msg.c_str());
    reply(res, code, msg);
}

// ---- 系统资源采样（/proc 直读，纯 Linux；~10s 周期前端轮询差分足够）----

struct CpuTimes {
    long long idle = 0, total = 0;
    bool valid = false;
};

// /proc/stat 第一行（cpu 汇总）：user nice system idle iowait irq softirq steal
CpuTimes read_cpu_times() {
    std::ifstream f("/proc/stat");
    std::string cpu;
    if (!(f >> cpu) || cpu != "cpu") return {};
    long long v[8] = {0};
    for (long long& x : v)
        if (!(f >> x)) return {};
    f.close();
    long long idle = v[3] + v[4];  // idle + iowait
    long long total = 0;
    for (long long x : v) total += x;
    return {idle, total, true};
}

// CPU 占用 = 两次采样间非 idle 时间占比（差分；首个采样周期返回 -1 表示未就绪）
double read_cpu_pct() {
    static CpuTimes last;
    const CpuTimes cur = read_cpu_times();
    if (!cur.valid || !last.valid) {
        last = cur;
        return -1.0;
    }
    const long long dt = cur.total - last.total;
    const long long di = cur.idle - last.idle;
    last = cur;
    if (dt <= 0) return -1.0;
    const double pct = 100.0 * static_cast<double>(dt - di) / static_cast<double>(dt);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

struct MemInfo {
    long long total_kb = 0, avail_kb = 0;
    bool valid = false;
};

// /proc/meminfo：MemTotal + MemAvailable（含可回收缓存，比 MemFree 更接近真实可用）
MemInfo read_mem_info() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    long long val = 0;
    std::string unit;
    MemInfo m;
    while (f >> key >> val) {
        std::getline(f, unit);  // 吃掉 " kB ..." 行尾
        if (key == "MemTotal:") m.total_kb = val;
        else if (key == "MemAvailable:") { m.avail_kb = val; break; }
    }
    m.valid = m.total_kb > 0 && m.avail_kb > 0;
    return m;
}

// /proc/<pid>/status VmRSS（kB）；pid 非法返回 0
long long read_vm_rss_kb(pid_t pid) {
    if (pid <= 0) return 0;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
    std::ifstream f(path);
    std::string key;
    long long val = 0;
    std::string unit;
    while (f >> key >> val) {
        std::getline(f, unit);
        if (key == "VmRSS:") return val;
    }
    return 0;
}

// ZLM 进程 RSS：兼容进程名变体（MediaServer/MediaServer_kernal/-master 等，
// 逐个 pgrep；也可用配置指定，V1 按名扫描即可）
long long read_zlm_rss_kb() {
    static const char* kNames[] = {"MediaServer", "MediaServer_kernal",
                                   "MediaServer-master"};
    for (const char* name : kNames) {
        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "pgrep -x %s 2>/dev/null | head -1", name);
        FILE* p = popen(cmd, "r");
        if (!p) continue;
        int pid = 0;
        const int n = fscanf(p, "%d", &pid);
        pclose(p);
        if (n == 1) {
            const long long kb = read_vm_rss_kb(static_cast<pid_t>(pid));
            if (kb > 0) return kb;
        }
    }
    return 0;
}

// 录像目录磁盘水位（statvfs 与 Janitor 同口径；异常返回 false）
bool read_disk_info(const std::string& dir, double* used_pct, double* total_gb,
                    double* free_gb) {
    struct statvfs st{};
    if (::statvfs(dir.c_str(), &st) != 0) return false;
    const unsigned long long total =
        static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
    const unsigned long long free_b =
        static_cast<unsigned long long>(st.f_bavail) * st.f_frsize;  // 普通用户可用
    if (total == 0) return false;
    *total_gb = static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0);
    *free_gb = static_cast<double>(free_b) / (1024.0 * 1024.0 * 1024.0);
    *used_pct = 100.0 * static_cast<double>(total - free_b) / static_cast<double>(total);
    return true;
}

// 请求 Host（stream/recordings URL 拼装用）；缺省 host 头时退回占位
std::string host_of(const httplib::Request& req) {
    const std::string h = req.get_header_value("Host");
    return h.empty() ? "127.0.0.1" : h;
}

// host_of 去端口（"1.2.3.4:8080" → "1.2.3.4"）；无冒号原样返回
std::string host_only(const std::string& host) {
    const size_t pos = host.rfind(':');
    if (pos == std::string::npos) return host;
    // IPv6 字面量 [::1]:8080 只截最后一个冒号也安全（本产品内网 IPv4 场景）
    return host.substr(0, pos);
}

// file_path → 文件名（recordings 直链拼装）
std::string basename_of(const std::string& p) {
    const size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

}  // namespace

// ---- NPU 预检（纯函数）----

namespace {

double compute_pct_of(const std::vector<PrecheckUsage>& usages,
                      const std::map<std::string, PrecheckModel>& models,
                      int npu_cores) {
    std::map<std::string, int> fps_sum;
    for (const auto& u : usages)
        if (u.fps > 0) fps_sum[u.algo_id] += u.fps;
    double cost = 0;
    for (const auto& kv : fps_sum) {
        const auto it = models.find(kv.first);
        if (it != models.end()) cost += it->second.cost_ms * kv.second;
    }
    const double denom = npu_cores * 1000.0 * 0.8;  // ms/s × 80% 水位
    return denom > 0 ? cost / denom * 100.0 : 0.0;
}

}  // namespace

PrecheckResult npu_precheck(const PrecheckInput& in) {
    std::map<std::string, PrecheckModel> models;
    for (const auto& m : in.models) models[m.algo_id] = m;

    PrecheckResult r;
    r.compute_pct = compute_pct_of(in.usages, models, in.npu_cores);

    // 内存：去重物理模型 Σ ctx
    std::map<std::string, int> seen;
    int64_t mem = 0;
    for (const auto& u : in.usages) {
        if (seen[u.algo_id]++) continue;
        const auto it = models.find(u.algo_id);
        if (it != models.end()) mem += it->second.mem_kb;
    }
    r.mem_pct = in.npu_mem_total_kb > 0
                    ? static_cast<double>(mem) /
                          (static_cast<double>(in.npu_mem_total_kb) * 0.8) * 100.0
                    : 0.0;

    r.ok = r.compute_pct <= 100.0 && r.mem_pct <= 100.0;
    if (r.ok) return r;

    if (r.compute_pct > 100.0) {
        r.code = 3001;
        // 建议：focus_cam 的勾选逐档降 fps（→3 / →1）试算，取首个达标档
        for (int target : {3, 1}) {
            std::vector<PrecheckUsage> trial = in.usages;
            for (auto& u : trial)
                if (u.cam == in.focus_cam && u.fps > target) u.fps = target;
            const double pct = compute_pct_of(trial, models, in.npu_cores);
            if (pct <= 100.0) {
                std::ostringstream os;
                os << "cam" << in.focus_cam << " fps →" << target
                   << " 可降至" << static_cast<int>(pct + 0.5) << "%";
                r.msg = os.str();
                return r;
            }
        }
        r.msg = "请降低检测 fps 或减少勾选通道";
    } else {
        r.code = 3002;
        r.msg = "NPU 内存预检超限，请减少勾选的模型数";
    }
    return r;
}

// ---- HttpApi ----

HttpApi::HttpApi(Deps d) : d_(std::move(d)), start_steady_ms_(steady_ms()) {}

HttpApi::~HttpApi() { stop(); }

bool HttpApi::start() {
    svr_ = std::make_unique<httplib::Server>();
    register_routes();
    if (!d_.static_dir.empty()) svr_->set_mount_point("/", d_.static_dir.c_str());
    if (!d_.recordings_dir.empty())
        svr_->set_mount_point("/recordings", d_.recordings_dir.c_str());
    if (!svr_->bind_to_port(d_.bind_host.c_str(), d_.http_port)) {
        LOGE("http-api", "bind %s:%d 失败", d_.bind_host.c_str(), d_.http_port);
        svr_.reset();
        return false;
    }
    port_ = d_.http_port;
    th_ = std::thread([this] { svr_->listen_after_bind(); });
    LOGI("http-api", "REST :%d ready（static=%s recordings=%s）", port_,
         d_.static_dir.c_str(), d_.recordings_dir.c_str());
    return true;
}

void HttpApi::stop() {
    if (svr_) svr_->stop();
    if (th_.joinable()) th_.join();
    svr_.reset();
}

bool HttpApi::auth_ok(const httplib::Request& req) const {
    if (d_.auth_token.empty()) return true;  // 首版局域网：无鉴权
    const std::string expect = "Bearer " + d_.auth_token;
    return req.get_header_value("Authorization") == expect;
}

// 预检输入收集：其他通道取 db 现状 + focus_cam 用本次请求覆盖
bool HttpApi::build_precheck_input(int cam, const std::vector<CamAlgoRow>& new_cfg,
                                   PrecheckInput* out, std::string* err) {
    for (const auto& m : d_.db->list_algorithms_meta()) {
        if (m.disabled) continue;
        out->models.push_back({m.algo_id, m.npu_cost_ms, m.npu_mem_kb});
    }
    for (const auto& c : d_.db->list_cameras(true)) {
        if (c.id == cam) continue;
        for (const auto& a : d_.db->list_camera_algos(c.id)) {
            if (!a.enabled) continue;
            out->usages.push_back({a.algo_id, static_cast<int>(c.id), a.detect_fps});
        }
    }
    for (const auto& a : new_cfg) {
        if (!a.enabled) continue;
        out->usages.push_back({a.algo_id, cam, a.detect_fps});
    }
    // 算法未注册 → engine 必拒 1002，预检前置拦截给出更明确错误
    for (const auto& a : new_cfg) {
        if (!a.enabled) continue;
        bool known = false;
        for (const auto& m : out->models)
            if (m.algo_id == a.algo_id) known = true;
        if (!known) {
            *err = "algo not available: " + a.algo_id;
            return false;
        }
    }
    out->focus_cam = cam;
    out->npu_cores = d_.npu_cores;
    out->npu_mem_total_kb = d_.npu_mem_total_kb;
    return true;
}

// ---- PUT /api/cameras/{id}（改名/改 rtsp/启停；详设 §5.4-3）----
// body 已由分发点解析（板上 httplib 可能截断 req.path，分发依据=请求体）

void HttpApi::handle_put_camera(const httplib::Request& req, httplib::Response& res,
                                int64_t id, const Json::Value& body) {
    (void)req;
    CameraRow cam;
    if (!d_.db->get_camera(id, cam)) {
        reply_err(res, 1002, "camera not found");
        return;
    }
    const std::string old_main = cam.rtsp_main, old_sub = cam.rtsp_sub;
    const bool old_enabled = cam.enabled;
    if (body.isMember("name")) cam.name = body["name"].asString();
    if (body.isMember("rtsp_main")) cam.rtsp_main = body["rtsp_main"].asString();
    if (body.isMember("rtsp_sub")) cam.rtsp_sub = body["rtsp_sub"].asString();
    if (body.isMember("enabled")) cam.enabled = body["enabled"].asInt() != 0;
    const bool rtsp_changed = cam.rtsp_main != old_main || cam.rtsp_sub != old_sub;
    const bool enabled_changed = cam.enabled != old_enabled;
    proto::SetAlgos algos;
    std::string compile_error;
    if (cam.enabled && (rtsp_changed || enabled_changed) &&
        !compile_algos(static_cast<uint32_t>(id), d_.db->list_camera_algos(id),
                       algos, compile_error)) {
        reply_err(res, 1001, compile_error);
        return;
    }

    // rtsp 变更：先删旧代理再建新（同名流不先删会被 ensure 幂等分支吞掉新 URL）
    if (rtsp_changed) {
        if (!cam.zlm_key_main.empty()) d_.zlm->del_proxy(cam.zlm_key_main);
        if (!cam.zlm_key_sub.empty()) d_.zlm->del_proxy(cam.zlm_key_sub);
        std::string km, ks;
        if (!d_.zlm->add_camera_proxies(static_cast<int>(id), cam.rtsp_main, cam.rtsp_sub,
                                        &km, &ks)) {
            reply_err(res, 4002, "ZLM 重建代理失败（旧代理已删，可重试或删除摄像头）");
            return;
        }
        if (!d_.db->set_camera_keys(id, km, ks)) {
            reply_err(res, 5001, "persist camera proxy keys failed");
            return;
        }
    }
    if (!d_.db->update_camera(cam)) {
        reply_err(res, 5001, "persist camera failed");
        return;
    }
    // engine 侧重建（enabled 时）
    if (rtsp_changed || enabled_changed) {
        proto::RemoveCam rc;
        rc.id = static_cast<uint32_t>(id);
        const auto removed = d_.uds->remove_cam(rc);
        if (removed.ret != 0 && removed.ret != 1002) {
            reply_err(res, removed.ret, "engine remove_cam failed: " + removed.msg);
            return;
        }
        if (cam.enabled) {
            proto::AddCam ac;
            ac.id = static_cast<uint32_t>(id);
            ac.rtsp_sub = cam.rtsp_sub;
            ac.rtsp_main = cam.rtsp_main;
            ac.has_rtsp_main = true;
            const proto::Ack ack = d_.uds->add_cam(ac);
            if (ack.ret != 0) {
                reply_err(res, ack.ret,
                          "engine add_cam 失败：" +
                              (ack.msg.empty() ? std::string("unknown") : ack.msg));
                return;
            }
            const auto applied = d_.uds->set_algos(algos);
            if (applied.ret != 0) {
                reply_err(res, applied.ret, "engine set_algos failed: " + applied.msg);
                return;
            }
        }
    }
    Json::Value data(Json::objectValue);
    data["ok"] = true;
    reply_data(res, data);
}

// ---- PUT /api/cameras/{id}/algos（预检闸门1，详设 §5.4 细节）----

void HttpApi::handle_put_algos(const httplib::Request& req, httplib::Response& res,
                               int64_t id, const Json::Value& body) {
    (void)req;
    CameraRow cam;
    if (!d_.db->get_camera(id, cam) || !cam.enabled) {
        reply_err(res, 1002, "camera not found or disabled");
        return;
    }
    if (!body.isMember("algos") || !body["algos"].isArray()) {
        reply_err(res, 1001, "需要 algos[]");
        return;
    }
    std::vector<CamAlgoRow> cfgs;
    for (const auto& a : body["algos"]) {
        if (!a.isObject() || !a["algo_id"].isString() ||
            (a.isMember("threshold") && !a["threshold"].isNumeric()) ||
            (a.isMember("fps") && !a["fps"].isInt()) ||
            (a.isMember("enabled") && !a["enabled"].isBool() && !a["enabled"].isInt())) {
            reply_err(res, 1001, "invalid algorithm fields");
            return;
        }
        CamAlgoRow r;
        r.algo_id = a["algo_id"].asString();
        r.threshold = a.get("threshold", 0.45).asDouble();
        r.detect_fps = a.get("fps", 3).asInt();
        if (r.detect_fps < 1 || r.detect_fps > 25) {
            reply_err(res, 1001, "fps 取值 1~25");
            return;
        }
        if (a.isMember("roi") && !a["roi"].isNull()) {
            r.roi_json = dump(a["roi"]);
            r.roi_json.erase(std::remove(r.roi_json.begin(), r.roi_json.end(), '\n'),
                             r.roi_json.end());
        }
        // 规则参数（事件接线）：随 roi 一起落库；未携带 = 不配规则（仅检测不报警）
        if (a.isMember("rule") && !a["rule"].isNull()) {
            r.rule_json = dump(a["rule"]);
            r.rule_json.erase(std::remove(r.rule_json.begin(), r.rule_json.end(), '\n'),
                              r.rule_json.end());
        }
        // 宫格 ROI 显示（v3 列）：display 缺省开（兼容老客户端）；color 校验
        // #rrggbb，非法/缺省落空串 = overlay 用默认主题色
        r.roi_display = a.get("roi_display", 1).asInt() != 0;
        if (a.isMember("roi_color") && a["roi_color"].isString()) {
            const std::string c = a["roi_color"].asString();
            bool ok = c.size() == 7 && c[0] == '#';
            for (size_t i = 1; ok && i < c.size(); ++i)
                ok = std::isxdigit(static_cast<unsigned char>(c[i])) != 0;
            if (ok) r.roi_color = c;
        }
        r.enabled = a.get("enabled", 1).asInt() != 0;
        cfgs.push_back(std::move(r));
    }
    proto::SetAlgos sa;
    std::string compile_error;
    if (!compile_algos(static_cast<uint32_t>(id), cfgs, sa, compile_error)) {
        reply_err(res, 1001, compile_error);
        return;
    }
    // 预检（闸门1，架构 §11.4.3）
    PrecheckInput pin;
    std::string perr;
    if (!build_precheck_input(static_cast<int>(id), cfgs, &pin, &perr)) {
        reply_err(res, 1002, perr);
        return;
    }
    const PrecheckResult pc = npu_precheck(pin);
    if (!pc.ok) {
        reply_err(res, pc.code, pc.msg);  // 3001/3002 + 建议文案（§4-T4.4 断言）
        return;
    }
    // 先 engine 后 DB：engine 拒绝（1002/1003）则 DB 不留悬空配置
    const proto::Ack ack = d_.uds->set_algos(sa);
    if (ack.ret != 0) {
        reply_err(res, ack.ret, ack.msg.empty() ? "engine set_algos 失败" : ack.msg);
        return;
    }
    if (!d_.db->set_camera_algos(id, cfgs)) {
        reply_err(res, 5001, "persist camera algorithms failed");
        return;
    }
    Json::Value data(Json::objectValue);
    data["ok"] = true;
    Json::Value est(Json::objectValue);
    est["compute_pct"] = pc.compute_pct;
    est["mem_pct"] = pc.mem_pct;
    data["npu_est"] = est;
    reply_data(res, data);
}

void HttpApi::register_routes() {
    // 统一鉴权 + 未匹配 API 记录
    // Generic callback supports board exception& and distro exception_ptr APIs.
    svr_->set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                   auto&& ep) {
        LOGE("http-api", "handler 异常: %s", http_exception_message(ep).c_str());
        reply_err(res, 5001, "internal error");
    });

    auto auth_wrap = [this](auto handler) {
        return [this, handler](const httplib::Request& req, httplib::Response& res) {
            if (!auth_ok(req)) {
                res.status = 401;
                reply_err(res, 1001, "unauthorized");
                return;
            }
            handler(req, res);
        };
    };

    // ---- 1. POST /api/cameras ----
    svr_->Post("/api/cameras", auth_wrap([this](const httplib::Request& req,
                                                httplib::Response& res) {
        Json::Value body;
        if (!parse_body(req.body, &body) || !body.isMember("name") ||
            !body.isMember("rtsp_main") || !body.isMember("rtsp_sub")) {
            reply_err(res, 1001, "需要 name/rtsp_main/rtsp_sub");
            return;
        }
        CameraRow cam;
        cam.name = body["name"].asString();
        cam.rtsp_main = body["rtsp_main"].asString();
        cam.rtsp_sub = body["rtsp_sub"].asString();
        cam.enabled = body.get("enabled", 1).asInt() != 0;
        if (cam.name.empty() || cam.rtsp_main.empty() || cam.rtsp_sub.empty()) {
            reply_err(res, 1001, "字段不能为空");
            return;
        }
        // 先占位落库（enabled=0 防 SYNCING 拾取半配置），代理编排成功再启用
        const int64_t id = d_.db->insert_camera(cam);
        if (id <= 0) {
            reply_err(res, 5001, "db insert failed");
            return;
        }
        std::string km, ks;
        if (!d_.zlm->add_camera_proxies(static_cast<int>(id), cam.rtsp_main,
                                        cam.rtsp_sub, &km, &ks)) {
            d_.db->delete_camera_cascade(id);  // 回滚（防半配置）
            reply_err(res, 4002, "ZLM 建代理失败（已回滚）");
            return;
        }
        d_.db->set_camera_keys(id, km, ks);
        // engine add_cam：失败不回滚（代理/记录保留，用户可重试或删除）
        proto::AddCam ac;
        ac.id = static_cast<uint32_t>(id);
        ac.rtsp_sub = cam.rtsp_sub;
        ac.rtsp_main = cam.rtsp_main;
        ac.has_rtsp_main = true;
        const proto::Ack ack = d_.uds->add_cam(ac);
        if (ack.ret != 0) {
            Json::Value data(Json::objectValue);
            data["id"] = id;
            data["note"] = "engine 添加失败（代理已建），可重试 enable 或删除";
            res.set_content(dump(envelope(ack.ret,
                                          ack.msg.empty() ? "engine add_cam 失败" : ack.msg,
                                          &data)),
                            "application/json");
            return;
        }
        cam.id = id;
        cam.enabled = true;
        d_.db->update_camera(cam);
        Json::Value data(Json::objectValue);
        data["id"] = id;
        Json::Value probe(Json::objectValue);
        Json::Value not_probed(Json::objectValue);
        not_probed["ok"] = false;
        not_probed["note"] = "V1 不做 ISAPI 预探测（详设 §5.4-1 可选项）";
        probe["main"] = not_probed;
        probe["sub"] = not_probed;
        data["probe"] = probe;
        reply_data(res, data);
    }));

    // ---- 2. GET /api/cameras ----
    svr_->Get("/api/cameras", auth_wrap([this](const httplib::Request&,
                                               httplib::Response& res) {
        Json::Value arr(Json::arrayValue);
        for (const auto& c : d_.db->list_cameras(true)) {
            Json::Value o(Json::objectValue);
            o["id"] = c.id;
            o["name"] = c.name;
            o["rtsp_main"] = c.rtsp_main;
            o["rtsp_sub"] = c.rtsp_sub;
            o["online"] = c.online;
            Json::Value algos(Json::arrayValue);
            for (const auto& a : d_.db->list_camera_algos(c.id)) {
                Json::Value ao(Json::objectValue);
                ao["algo_id"] = a.algo_id;
                ao["threshold"] = a.threshold;
                ao["fps"] = a.detect_fps;
                ao["enabled"] = a.enabled;
                // roi/rule 回显（配置页 ROI 绘制回显用）；未配置落 null
                Json::Value roi;
                if (parse_db_json(a.roi_json, &roi)) ao["roi"] = roi;
                else ao["roi"] = Json::Value(Json::nullValue);
                Json::Value rule;
                if (parse_db_json(a.rule_json, &rule)) ao["rule"] = rule;
                else ao["rule"] = Json::Value(Json::nullValue);
                ao["roi_display"] = a.roi_display != 0;  // 宫格显示开关（缺省开）
                if (!a.roi_color.empty()) ao["roi_color"] = a.roi_color;
                algos.append(ao);
            }
            o["algos"] = algos;
            arr.append(o);
        }
        Json::Value data;
        data = arr;
        reply_data(res, data);
    }));

    // ---- 2b. POST /api/cameras/scan（组播 + HTTP 网段兜底并行，阻塞 ~3~5s）----
    svr_->Post("/api/cameras/scan", auth_wrap([](const httplib::Request&,
                                                 httplib::Response& res) {
        const std::vector<DiscoveredDevice> devs = ws_discovery_probe(3000);
        LOGI("http-api", "WS-Discovery scan: %zu devices", devs.size());
        Json::Value arr(Json::arrayValue);
        for (const auto& d : devs) {
            Json::Value o(Json::objectValue);
            o["ip"] = d.ip;
            o["xaddr"] = d.xaddr;
            o["kind"] = d.kind;
            o["hik"] = d.hik;
            arr.append(o);
        }
        Json::Value data(Json::objectValue);
        data["devices"] = arr;
        reply_data(res, data);
    }));

    // ---- 2c. POST /api/cameras/onvif_uri（ONVIF GetStreamUri：账密 → 主/子码流）----
    svr_->Post("/api/cameras/onvif_uri", auth_wrap([](const httplib::Request& req,
                                                      httplib::Response& res) {
        Json::Value body;
        if (!parse_body(req.body, &body) || !body.isMember("ip") ||
            !body.isMember("xaddr") || !body.isMember("username") ||
            !body.isMember("password")) {
            reply_err(res, 1001, "需要 ip/xaddr/username/password");
            return;
        }
        int rc = 0;
        const OnvifUris u = onvif_get_stream_uris(
            body["ip"].asString(), body["xaddr"].asString(),
            body["username"].asString(), body["password"].asString(), 9000, &rc);
        if (rc != 0) {
            reply_err(res, rc,
                      rc == 2002 ? "认证失败，请检查用户名/密码" : "ONVIF 取流失败");
            return;
        }
        Json::Value data(Json::objectValue);
        data["rtsp_main"] = u.rtsp_main;
        data["rtsp_sub"] = u.rtsp_sub;
        data["profiles"] = u.profiles;
        reply_data(res, data);
    }));

    // ---- 3+5. PUT /api/cameras/{id} 与 /{id}/algos（两个显式路由 + body 分发）----
    // 板上 httplib 为 match（全串匹配）语义（板上实测）：
    //   "(\d+)" 不命中 "/2/algos"（404 空体）；"/stream" 能命中是因为模式里显式写了。
    //   早期版本疑似 search 语义的 "ok" 应答，实为当时通用路由误命中其他模式。
    // 双路由注册对 match/search 两种语义均正确：match 下各自精确命中；
    // search 下 "/algos" 更长更具体。分发仍按 body（camera 恒无 "algos"，
    // algos 恒含 "algos" 数组，互斥完备）——与路由引擎行为二次解耦
    auto put_dispatch = [this](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::atoll(req.matches[1].str().c_str());
        Json::Value body;
        if (!parse_body(req.body, &body)) {
            reply_err(res, 1001, "bad json");
            return;
        }
        if (body.isMember("algos") && body["algos"].isArray()) {
            handle_put_algos(req, res, id, body);
        } else {
            handle_put_camera(req, res, id, body);
        }
    };
    svr_->Put(R"(/api/cameras/(\d+)/algos)",
              auth_wrap([put_dispatch](const httplib::Request& req,
                                       httplib::Response& res) { put_dispatch(req, res); }));
    svr_->Put(R"(/api/cameras/(\d+))",
              auth_wrap([put_dispatch](const httplib::Request& req,
                                       httplib::Response& res) { put_dispatch(req, res); }));

    // ---- 4. DELETE /api/cameras/{id} ----
    svr_->Delete(R"(/api/cameras/(\d+))",
                 auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::atoll(req.matches[1].str().c_str());
        CameraRow cam;
        if (!d_.db->get_camera(id, cam)) {
            reply_err(res, 1002, "camera not found");
            return;
        }
        proto::RemoveCam rc;
        rc.id = static_cast<uint32_t>(id);
        d_.uds->remove_cam(rc);  // best-effort
        if (!cam.zlm_key_main.empty()) d_.zlm->del_proxy(cam.zlm_key_main);
        if (!cam.zlm_key_sub.empty()) d_.zlm->del_proxy(cam.zlm_key_sub);
        d_.db->logical_delete_camera(id);  // 详设 §5.4-4：逻辑删（保留关联）
        Json::Value data(Json::objectValue);
        data["ok"] = true;
        reply_data(res, data);
    }));

    // ---- 6. GET /api/cameras/{id}/stream ----
    svr_->Get(R"(/api/cameras/(\d+)/stream)",
              auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        const int id = std::atoi(req.matches[1].str().c_str());
        std::string quality = "sub";
        if (req.has_param("quality")) quality = req.get_param_value("quality");
        if (quality != "sub" && quality != "main") {
            reply_err(res, 1001, "quality 取值 sub|main");
            return;
        }
        const std::string stream = "cam" + std::to_string(id) + "_" + quality;
        // WebRTC 信令在 ZLM HTTP 服务（配置 zlm.port），不在 businessd 自身端口：
        // host 用浏览器请求的 Host（保证浏览器可达），端口用 ZLM 实际监听口
        const int zlm_port = d_.zlm ? d_.zlm->port() : 80;
        Json::Value data(Json::objectValue);
        data["type"] = "webrtc";
        data["signaling"] = "http://" + host_only(host_of(req)) +
                            (zlm_port != 80 ? ":" + std::to_string(zlm_port) : "") +
                            "/index/api/webrtc?app=live&stream=" + stream + "&type=play";
        data["app"] = "live";
        data["stream"] = stream;
        reply_data(res, data);
    }));

    // ---- 6b. GET /api/cameras/{id}/snapshot（ROI 编辑底图：ZLM getSnap 单帧 JPEG）----
    // 二进制端点（不走 envelope）：成功 image/jpeg；失败 JSON 错误（前端 img.onerror 兜底）
    svr_->Get(R"(/api/cameras/(\d+)/snapshot)",
              auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        const int id = std::atoi(req.matches[1].str().c_str());
        CameraRow cam;
        if (!d_.db->get_camera(id, cam) || !cam.enabled) {
            reply_err(res, 1002, "camera not found or disabled");
            return;
        }
        if (!d_.zlm) {
            reply_err(res, 4002, "zlm 不可用");
            return;
        }
        std::string jpeg;
        // getSnap 拉摄像头子码流首帧最多等 8s；快照与检测画面同源（子码流），
        // ROI 归一化坐标与主/子码流分辨率解耦
        if (!d_.zlm->get_snap(cam.rtsp_sub, 8, &jpeg)) {
            reply_err(res, 2001, "截图失败（通道离线或 ZLM 超时）");
            return;
        }
        res.set_content(jpeg, "image/jpeg");
    }));

    // ---- 7. GET /api/algorithms ----
    svr_->Get("/api/algorithms",
              auth_wrap([this](const httplib::Request&, httplib::Response& res) {
        Json::Value arr(Json::arrayValue);
        for (const auto& m : d_.db->list_algorithms_meta()) {
            Json::Value o(Json::objectValue);
            o["algo_id"] = m.algo_id;
            o["name"] = m.name;
            o["version"] = m.version;
            o["model_ref"] = m.model_ref;
            o["trigger"] = m.trigger_type;
            o["npu_cost_ms"] = m.npu_cost_ms;
            o["npu_mem_kb"] = static_cast<int>(m.npu_mem_kb);
            o["disabled"] = m.disabled;
            Json::CharReaderBuilder b;
            std::unique_ptr<Json::CharReader> rd(b.newCharReader());
            Json::Value classes;
            std::string errs;
            if (rd->parse(m.classes.data(), m.classes.data() + m.classes.size(),
                          &classes, &errs)) {
                o["classes"] = classes;
            } else {
                o["classes"] = Json::Value(Json::arrayValue);
            }
            arr.append(o);
        }
        Json::Value data;
        data = arr;
        reply_data(res, data);
    }));

    // ---- 8. GET /api/recordings ----
    svr_->Get("/api/recordings",
              auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        const int64_t camera_id =
            req.has_param("camera_id") ? std::atoll(req.get_param_value("camera_id").c_str()) : 0;
        const int64_t from = req.has_param("from")
                                 ? std::atoll(req.get_param_value("from").c_str())
                                 : 0;
        const int64_t to = req.has_param("to") ? std::atoll(req.get_param_value("to").c_str()) : 0;
        const int limit = req.has_param("limit")
                              ? std::atoi(req.get_param_value("limit").c_str())
                              : 100;
        const auto segs = d_.db->list_segments(camera_id, from, to, limit);
        const std::string host = host_of(req);
        Json::Value items(Json::arrayValue);
        for (const auto& s : segs) {
            Json::Value o(Json::objectValue);
            o["id"] = s.id;
            o["camera_id"] = s.camera_id;
            o["file_path"] = s.file_path;
            o["start_ts"] = s.start_ts;
            o["duration_s"] = s.duration_s;
            o["url"] = "http://" + host + "/recordings/" + basename_of(s.file_path);
            items.append(o);
        }
        Json::Value data(Json::objectValue);
        data["total"] = static_cast<int>(items.size());
        data["items"] = items;
        reply_data(res, data);
    }));

    // ---- 9. GET /api/events ----
    svr_->Get("/api/events",
              auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        const int64_t camera_id =
            req.has_param("camera_id") ? std::atoll(req.get_param_value("camera_id").c_str()) : 0;
        const std::string algo = req.has_param("algo_id")
                                     ? req.get_param_value("algo_id")
                                     : "";
        const int64_t from = req.has_param("from")
                                 ? std::atoll(req.get_param_value("from").c_str())
                                 : 0;
        const int64_t to = req.has_param("to") ? std::atoll(req.get_param_value("to").c_str()) : 0;
        const int limit = req.has_param("limit")
                              ? std::atoi(req.get_param_value("limit").c_str())
                              : 100;
        const auto evs = d_.db->list_events(camera_id, algo, from, to, limit);
        Json::Value arr(Json::arrayValue);
        for (const auto& e : evs) {
            Json::Value o(Json::objectValue);
            o["id"] = e.id;
            o["camera_id"] = e.camera_id;
            o["algo_id"] = e.algo_id;
            o["type"] = e.type;
            o["ts"] = e.ts;
            if (!e.snapshot_path.empty()) o["snapshot"] = e.snapshot_path;
            Json::CharReaderBuilder b;
            std::unique_ptr<Json::CharReader> rd(b.newCharReader());
            Json::Value targets;
            std::string errs;
            if (rd->parse(e.targets_json.data(),
                          e.targets_json.data() + e.targets_json.size(), &targets,
                          &errs)) {
                o["targets"] = targets;
            }
            arr.append(o);
        }
        Json::Value data;
        data = arr;
        reply_data(res, data);
    }));

    // ---- 10. GET /api/resources（get_metrics 透传 + 本机 CPU/内存/RSS 实测）----
    svr_->Get("/api/resources",
              auth_wrap([this](const httplib::Request&, httplib::Response& res) {
        Json::Value data;
        std::string metrics;
        if (d_.uds->get_metrics(&metrics, 3000)) {
            Json::CharReaderBuilder b;
            std::unique_ptr<Json::CharReader> rd(b.newCharReader());
            std::string errs;
            if (rd->parse(metrics.data(), metrics.data() + metrics.size(), &data,
                          &errs)) {
                data["engine_online"] = true;
            } else {
                data["engine_online"] = false;
            }
        } else {
            data["engine_online"] = false;
        }
        // 本机系统指标（businessd 自采，不依赖 engine）：
        //   cpu_pct：两次 /api/resources 轮询间的差分（前端 10s 周期正合适；
        //            首次请求返回 -1，前端显示 "--"）
        const double cpu_pct = read_cpu_pct();
        data["cpu_pct"] = cpu_pct;
        const MemInfo mem = read_mem_info();
        Json::Value memj(Json::objectValue);
        memj["total_mb"] = static_cast<Json::Int64>(mem.total_kb / 1024);
        memj["used_pct"] =
            mem.valid
                ? 100.0 * static_cast<double>(mem.total_kb - mem.avail_kb) /
                      static_cast<double>(mem.total_kb)
                : -1.0;
        data["mem"] = memj;
        // 进程 RSS（kB → MB）：self=getpid；engine pid 经 DB 未存，V1 先 self+zlm
        Json::Value rss(Json::objectValue);
        rss["businessd"] =
            static_cast<Json::Int64>(read_vm_rss_kb(getpid()) / 1024);
        rss["zlm"] = static_cast<Json::Int64>(read_zlm_rss_kb() / 1024);
        data["rss"] = rss;
        reply_data(res, data);
    }));

    // ---- 11. GET /api/system/info ----
    svr_->Get("/api/system/info",
              auth_wrap([this](const httplib::Request&, httplib::Response& res) {
        Json::Value data(Json::objectValue);
        data["version"] = d_.version;
        data["ready"] = ready_.load(std::memory_order_relaxed);
        data["uptime_s"] = static_cast<int>((steady_ms() - start_steady_ms_) / 1000);
        const auto cams = d_.db->list_cameras(true);
        int online = 0;
        for (const auto& c : cams)
            if (c.online) ++online;
        data["cameras_online"] = online;
        data["cameras_total"] = static_cast<int>(cams.size());
        data["ws_port"] = d_.ws_port;
        // 录像目录磁盘水位实测（Janitor 90% 兜底同口径；空目录 = 全盘）
        Json::Value disk(Json::objectValue);
        double up = -1, tg = 0, fg = 0;
        const std::string target = d_.recordings_dir.empty() ? "." : d_.recordings_dir;
        if (read_disk_info(target, &up, &tg, &fg)) {
            disk["total_gb"] = tg;
            disk["free_gb"] = fg;
            disk["used_pct"] = up;
        } else {
            disk["total_gb"] = 0;
            disk["free_gb"] = 0;
            disk["used_pct"] = -1;
        }
        disk["retention_days_est"] = 0;
        data["disk"] = disk;
        reply_data(res, data);
    }));

    // ---- 12. POST /api/algorithms/{id}/rollback ----
    svr_->Post(R"(/api/algorithms/([^/]+)/rollback)",
               auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        const std::string algo = req.matches[1].str();
        proto::ReloadAlgo rl;
        rl.algo = algo;
        const proto::Ack ack = d_.uds->reload_algo(rl);
        if (ack.ret != 0) {
            // 3004 = 自检失败已回滚（详设 §8.2），语义上属于"回滚动作已执行"
            reply_err(res, ack.ret, ack.msg.empty() ? "reload failed" : ack.msg);
            return;
        }
        Json::Value data(Json::objectValue);
        data["ok"] = true;
        reply_data(res, data);
    }));
}

}  // namespace biz
}  // namespace aibox
