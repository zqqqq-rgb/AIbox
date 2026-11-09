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
#include <limits>
#include <sstream>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/Log.h"
#include "config_compiler.h"
#include "camera_service.h"
#include "http_compat.h"
#include "onvif_client.h"
#include "uds_client.h"
#include "zlm_client.h"
#include "ws_server.h"
#include "task_service.h"
#include "access_session.h"

namespace aibox {
namespace biz {

namespace {

int64_t parse_id(const std::string& text) {
    int64_t value=0;
    for(char ch:text) {
        if(ch<'0'||ch>'9'||value>(std::numeric_limits<int64_t>::max()-(ch-'0'))/10) return -1;
        value=value*10+(ch-'0');
    }
    return value>0?value:-1;
}

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

bool parse_task_body(const std::string& body, Json::Value* out) {
    if(body.size()>2*1024*1024) return false;
    Json::CharReaderBuilder builder;
    builder["failIfExtra"]=true; builder["rejectDupKeys"]=true; builder["allowComments"]=false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader()); std::string error;
    return reader->parse(body.data(),body.data()+body.size(),out,&error);
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

void reply_configuration(httplib::Response& res, const ConfigurationResult& result, bool created = false) {
    Json::Value data;
    data["id"] = static_cast<Json::Int64>(result.camera_id);
    data["ok"] = result.code == 0;
    data["accepted"] = result.accepted;
    data["operation_id"] = result.operation_id;
    data["target_revision"] = static_cast<Json::Int64>(result.revision);
    data["apply_status"] = result.apply_status;
    data["pending"] = result.accepted && result.apply_status != "succeeded" &&
                      result.apply_status != "failed" && result.apply_status != "superseded";
    data["npu_est"]["compute_pct"] = result.compute_pct;
    data["npu_est"]["mem_pct"] = result.mem_pct;
    if (created) {
        Json::Value probe;
        probe["ok"] = false;
        probe["note"] = "未执行码流预探测";
        data["probe"]["main"] = probe;
        data["probe"]["sub"] = probe;
    }
    reply(res, result.code, result.message, &data);
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

// ---- HttpApi ----

HttpApi::HttpApi(Deps d) : d_(std::move(d)), start_steady_ms_(steady_ms()) {
    RemoteAccessConfig remote{d_.public_base_url, d_.public_ws_url, d_.public_media_signaling_path};
    std::string remote_error;
    if (validate_remote_access(remote, d_.auth_token, remote_error)) {
        d_.public_base_url = remote.public_base_url;
        d_.public_ws_url = remote.public_ws_url;
    }
    if (!d_.access) {
        AccessSession::Params access;
        access.token = d_.auth_token;
        access.public_origin = d_.public_base_url;
        access.http_port = d_.http_port;
        d_.access = std::make_shared<AccessSession>(std::move(access));
    }
    CameraService::Params params;
    params.reconciler.media_host = d_.media_host;
    params.reconciler.media_port = d_.media_rtsp_port;
    params.reconciler.retry_ms = d_.configuration_retry_ms;
    params.npu_cores = d_.npu_cores;
    params.npu_mem_kb = d_.npu_mem_total_kb;
    params.apply_wait_ms = d_.configuration_wait_ms;
    cameras_ = std::make_unique<CameraService>(*d_.db, *d_.uds, *d_.zlm, params);
}

HttpApi::~HttpApi() { stop(); }

bool HttpApi::start() {
    if (th_.joinable()) return true;
    RemoteAccessConfig remote{d_.public_base_url, d_.public_ws_url, d_.public_media_signaling_path};
    std::string config_error;
    if (!validate_remote_access(remote, d_.auth_token, config_error)) {
        LOGE("http-api", "invalid remote access configuration: %s", config_error.c_str());
        return false;
    }
    d_.public_base_url = remote.public_base_url;
    d_.public_ws_url = remote.public_ws_url;
    stopping_ = false;
    svr_ = std::make_unique<httplib::Server>();
    svr_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        const bool api = req.path == "/api" || req.path.rfind("/api/", 0) == 0;
        const bool evidence = req.path == "/recordings" || req.path.rfind("/recordings/", 0) == 0;
        const bool public_auth = req.path == "/api/auth/status" || req.path == "/api/auth/login";
        if ((api || evidence) && !public_auth) {
            if (!auth_ok(req)) {
                res.status = 401; reply_err(res, 1001, "authentication required");
                res.set_header("Cache-Control", "no-store");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (d_.access->required() && !d_.access->bearer_valid(req.get_header_value("Authorization")) &&
                req.method != "GET" && req.method != "HEAD" && req.method != "OPTIONS" &&
                !d_.access->origin_allowed(req.get_header_value("Origin"), req.get_header_value("Host"))) {
                res.status = 403; reply_err(res, 1001, "request origin is not allowed");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (d_.access->required()) res.set_header("Cache-Control", "private, no-store");
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    register_routes();
    if (!d_.static_dir.empty()) svr_->set_mount_point("/", d_.static_dir.c_str());
    if (!d_.recordings_dir.empty())
        svr_->set_mount_point("/recordings", d_.recordings_dir.c_str());
    if (!svr_->bind_to_port(d_.bind_host.c_str(), d_.http_port)) {
        LOGE("http-api", "bind %s:%d 失败", d_.bind_host.c_str(), d_.http_port);
        svr_.reset();
        return false;
    }
    if (!cameras_->start()) { svr_.reset(); return false; }
    port_ = d_.http_port;
    th_ = std::thread([this] { svr_->listen_after_bind(); });
    LOGI("http-api", "REST :%d ready（static=%s recordings=%s）", port_,
         d_.static_dir.c_str(), d_.recordings_dir.c_str());
    return true;
}

void HttpApi::stop() {
    begin_shutdown();
    if (svr_) svr_->stop();
    if (th_.joinable()) th_.join();
    cameras_->stop();
    svr_.reset();
}

void HttpApi::begin_shutdown() {
    stopping_ = true;
    ready_ = false;
    cameras_->begin_shutdown();
}

bool HttpApi::auth_ok(const httplib::Request& req) const {
    return d_.access->authenticated(req.get_header_value("Authorization"), req.get_header_value("Cookie"));
}

// 预检输入收集：其他通道取 db 现状 + focus_cam 用本次请求覆盖
// ---- PUT /api/cameras/{id}（改名/改 rtsp/启停；详设 §5.4-3）----
// body 已由分发点解析（板上 httplib 可能截断 req.path，分发依据=请求体）

void HttpApi::handle_put_camera(const httplib::Request& req, httplib::Response& res,
                                int64_t id, const Json::Value& body) {
    (void)req;
    if (!body.isObject()) { reply_err(res, 1001, "expected camera object"); return; }
    CameraPatch patch;
    for (const char* key : {"name", "rtsp_main", "rtsp_sub"})
        if (body.isMember(key) && !body[key].isString()) {
            reply_err(res, 1001, "invalid camera field"); return;
        }
    if (body.isMember("name")) patch.name = body["name"].asString();
    if (body.isMember("rtsp_main")) patch.rtsp_main = body["rtsp_main"].asString();
    if (body.isMember("rtsp_sub")) patch.rtsp_sub = body["rtsp_sub"].asString();
    if (body.isMember("enabled")) {
        if (!body["enabled"].isBool() && !body["enabled"].isInt()) {
            reply_err(res, 1001, "invalid enabled field"); return;
        }
        patch.enabled = body["enabled"].asInt() != 0;
    }
    reply_configuration(res, cameras_->update(id, patch));
}

// ---- PUT /api/cameras/{id}/algos（预检闸门1，详设 §5.4 细节）----

void HttpApi::handle_put_algos(const httplib::Request& req, httplib::Response& res,
                               int64_t id, const Json::Value& body) {
    (void)req;
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
    reply_configuration(res, cameras_->set_algos(id, cfgs));
}

void HttpApi::register_routes() {
    svr_->Get("/api/auth/status", [this](const httplib::Request& req, httplib::Response& res) {
        Json::Value data;
        data["auth_required"] = d_.access->required();
        data["authenticated"] = auth_ok(req);
        res.set_header("Cache-Control", "no-store");
        reply_data(res, data);
    });
    svr_->Post("/api/auth/login", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (!d_.access->origin_allowed(req.get_header_value("Origin"), req.get_header_value("Host"), true)) {
            res.status = 403; reply_err(res, 1001, "request origin is not allowed"); return;
        }
        Json::Value body;
        if (req.body.size() > 4096 || !parse_task_body(req.body, &body) || !body.isObject() ||
            body.size() != 1 || !body["token"].isString() || body["token"].asString().size() > 2048) {
            reply_err(res, 1001, "expected a token string"); return;
        }
        if (!d_.access->required()) {
            Json::Value data; data["authenticated"] = true; data["auth_required"] = false;
            reply_data(res, data); return;
        }
        const std::string id = d_.access->login(body["token"].asString());
        if (id.empty()) { res.status = 401; reply_err(res, 1001, "invalid credential or session limit reached"); return; }
        d_.access->logout(req.get_header_value("Cookie"));
        res.set_header("Set-Cookie", d_.access->cookie_header(id));
        Json::Value data; data["authenticated"] = true; data["auth_required"] = true;
        reply_data(res, data);
    });
    svr_->Post("/api/auth/logout", [this](const httplib::Request& req, httplib::Response& res) {
        d_.access->logout(req.get_header_value("Cookie"));
        res.set_header("Set-Cookie", d_.access->clear_cookie_header());
        res.set_header("Cache-Control", "no-store");
        Json::Value data; data["authenticated"] = !d_.access->required();
        reply_data(res, data);
    });
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
            if (stopping_ && req.method != "GET" && req.method != "HEAD" && req.method != "OPTIONS") {
                Json::Value data(Json::objectValue);
                data["accepted"] = false;
                reply(res, 1003, "businessd is stopping", &data);
                return;
            }
            handler(req, res);
        };
    };

    svr_->Get("/api/auth/check", auth_wrap([](const httplib::Request&, httplib::Response& res) {
        Json::Value data; data["authenticated"] = true;
        res.set_header("Cache-Control", "no-store"); reply_data(res, data);
    }));

    // Task drafts and previews do not send engine commands. Legacy writes keep using CameraService.
    auto task_reply=[](httplib::Response& res,const TaskResult& result) { reply(res,result.code,result.message,&result.data); };
    svr_->Get("/api/capabilities",auth_wrap([this](const httplib::Request&,httplib::Response& res) {
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); reply_data(res,tasks.capabilities());
    }));
    svr_->Get("/api/tasks",auth_wrap([this](const httplib::Request&,httplib::Response& res) {
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); reply_data(res,tasks.list());
    }));
    svr_->Get(R"(/api/tasks/(\d+))",auth_wrap([this,task_reply](const httplib::Request& req,httplib::Response& res) {
        const auto id=parse_id(req.matches[1].str());
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); task_reply(res,tasks.get(id));
    }));
    svr_->Post("/api/tasks",auth_wrap([this,task_reply](const httplib::Request& req,httplib::Response& res) {
        Json::Value body; if(!parse_task_body(req.body,&body)) { reply_err(res,1001,"invalid task JSON or body over 2 MiB"); return; }
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); task_reply(res,tasks.save(0,body));
    }));
    svr_->Put(R"(/api/tasks/(\d+))",auth_wrap([this,task_reply](const httplib::Request& req,httplib::Response& res) {
        Json::Value body; if(!parse_task_body(req.body,&body)) { reply_err(res,1001,"invalid task JSON or body over 2 MiB"); return; }
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); task_reply(res,tasks.save(parse_id(req.matches[1].str()),body));
    }));
    svr_->Delete(R"(/api/tasks/(\d+))",auth_wrap([this,task_reply](const httplib::Request& req,httplib::Response& res) {
        Json::Value body; if(!parse_task_body(req.body,&body)) { reply_err(res,1001,"expected task revision"); return; }
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); task_reply(res,tasks.remove(parse_id(req.matches[1].str()),body));
    }));
    svr_->Put(R"(/api/tasks/(\d+)/enabled)",auth_wrap([this,task_reply](const httplib::Request& req,httplib::Response& res) {
        Json::Value body;
        if(!parse_task_body(req.body,&body)) { reply_err(res,1001,"invalid task transition JSON"); return; }
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get());
        task_reply(res,tasks.set_enabled(parse_id(req.matches[1].str()),body));
    }));
    svr_->Post("/api/tasks/preview",auth_wrap([this,task_reply](const httplib::Request& req,httplib::Response& res) {
        Json::Value body; if(!parse_task_body(req.body,&body)) { reply_err(res,1001,"invalid preview JSON or body over 2 MiB"); return; }
        TaskService tasks(*d_.db,d_.npu_cores,d_.npu_mem_total_kb,cameras_.get()); task_reply(res,tasks.preview(body));
    }));

    svr_->Post("/api/cameras", auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        Json::Value body;
        if (!parse_body(req.body, &body) || !body.isObject() ||
            !body["name"].isString() || !body["rtsp_main"].isString() || !body["rtsp_sub"].isString() ||
            (body.isMember("enabled") && !body["enabled"].isBool() && !body["enabled"].isInt())) {
            reply_err(res, 1001, "expected name/rtsp_main/rtsp_sub"); return;
        }
        CameraRow camera;
        camera.name = body["name"].asString();
        camera.rtsp_main = body["rtsp_main"].asString();
        camera.rtsp_sub = body["rtsp_sub"].asString();
        camera.enabled = body.get("enabled", true).asInt() != 0;
        reply_configuration(res, cameras_->create(camera), true);
    }));

    // ---- 2. GET /api/cameras ----
    svr_->Get("/api/cameras", auth_wrap([this](const httplib::Request&,
                                               httplib::Response& res) {
        Json::Value arr(Json::arrayValue);
        for (const auto& c : d_.db->list_cameras(false)) {
            Json::Value o(Json::objectValue);
            o["id"] = c.id;
            o["name"] = c.name;
            o["rtsp_main"] = c.rtsp_main;
            o["rtsp_sub"] = c.rtsp_sub;
            o["online"] = c.enabled && c.online && d_.uds->online();
            o["enabled"] = c.enabled;
            CameraConfigState state;
            if (d_.db->get_camera_config_state(c.id, state)) {
                o["desired_revision"] = static_cast<Json::Int64>(state.desired_revision);
                o["applied_revision"] = static_cast<Json::Int64>(state.applied_revision);
                o["apply_status"] = state.apply_status;
                o["last_error"] = state.last_error;
                o["runtime_state"] = d_.uds->online() && state.apply_status == "succeeded" ? "confirmed" : "unknown";
            }
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

    svr_->Get("/api/cameras/scan/interfaces", auth_wrap([](const httplib::Request&, httplib::Response& res) {
        Json::Value data; data["interfaces"] = Json::Value(Json::arrayValue);
        for (const auto& interface : enumerate_discovery_interfaces()) {
            Json::Value row;
            row["name"] = interface.name; row["address"] = interface.address;
            row["netmask"] = interface.netmask; row["cidr"] = interface.cidr;
            data["interfaces"].append(row);
        }
        reply_data(res, data);
    }));
    // A single bounded scan can run at a time; concurrent requests are rejected
    // rather than accumulating 32-worker sweeps in the HTTP thread pool.
    svr_->Post("/api/cameras/scan", auth_wrap([this](const httplib::Request& req,
                                                  httplib::Response& res) {
        Json::Value body(Json::objectValue);
        if ((!req.body.empty() && (req.body.size() > 4096 || !parse_task_body(req.body, &body))) || !body.isObject()) {
            reply_err(res, 1001, "expected scan options object"); return;
        }
        for (const auto& name : body.getMemberNames()) {
            if (name != "interface" && name != "cidr" && name != "timeout_ms") {
                reply_err(res, 1001, "unknown scan option"); return;
            }
        }
        if ((body.isMember("interface") && (!body["interface"].isString() || body["interface"].asString().size() > 64)) ||
            (body.isMember("cidr") && (!body["cidr"].isString() || body["cidr"].asString().size() > 32)) ||
            (body.isMember("timeout_ms") && (!body["timeout_ms"].isInt() || body["timeout_ms"].asInt() < 500 || body["timeout_ms"].asInt() > 10000))) {
            reply_err(res, 1001, "invalid scan options"); return;
        }
        DiscoveryOptions options;
        options.interface_name = body.get("interface", "").asString();
        options.cidr = body.get("cidr", "").asString();
        options.timeout_ms = body.get("timeout_ms", 3000).asInt();
        std::unique_lock<std::mutex> scan_lock(scan_mu_, std::try_to_lock);
        if (!scan_lock.owns_lock()) { reply_err(res, 1003, "camera scan already running"); return; }
        const auto result = discover_cameras(options);
        Json::Value arr(Json::arrayValue);
        for (const auto& d : result.devices) {
            Json::Value o(Json::objectValue);
            o["ip"] = d.ip;
            o["xaddr"] = d.xaddr;
            o["kind"] = d.kind;
            o["hik"] = d.hik;
            o["source"] = d.source;
            o["confirmed"] = d.confirmed;
            arr.append(o);
        }
        Json::Value data(Json::objectValue);
        data["devices"] = arr;
        data["target_count"] = Json::UInt64(result.target_count);
        data["attempted_count"] = Json::UInt64(result.attempted_count);
        data["elapsed_ms"] = result.elapsed_ms;
        data["timed_out"] = result.timed_out;
        data["warnings"] = Json::Value(Json::arrayValue);
        for (const auto& warning : result.warnings) data["warnings"].append(warning);
        reply(res, result.code, result.message, &data);
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
        if (!parse_body(req.body, &body) || !body.isObject()) {
            reply_err(res, 1001, "bad json");
            return;
        }
        if (body.isMember("algos") || req.path.find("/algos") != std::string::npos) {
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

    svr_->Delete(R"(/api/cameras/(\d+))", auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        reply_configuration(res, cameras_->remove(std::atoll(req.matches[1].str().c_str())));
    }));
    svr_->Get(R"(/api/operations/([A-Za-z0-9_-]+))", auth_wrap([this](const httplib::Request& req, httplib::Response& res) {
        ConfigOperationRow operation;
        if (!d_.db->get_config_operation(req.matches[1].str(), operation)) { reply_err(res, 1002, "operation not found"); return; }
        Json::Value data;
        data["operation_id"] = operation.operation_id;
        data["camera_id"] = static_cast<Json::Int64>(operation.camera_id);
        data["target_revision"] = static_cast<Json::Int64>(operation.target_revision);
        data["type"] = operation.op_type;
        data["status"] = operation.status;
        data["attempts"] = operation.attempts;
        data["error_code"] = operation.error_code;
        data["last_error"] = operation.last_error;
        data["next_retry_at"] = static_cast<Json::Int64>(operation.next_retry_at);
        data["created_at"] = static_cast<Json::Int64>(operation.created_at);
        data["updated_at"] = static_cast<Json::Int64>(operation.updated_at);
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
        const std::string signaling = d_.public_base_url.empty()
            ? "http://" + host_only(host_of(req)) + (zlm_port != 80 ? ":" + std::to_string(zlm_port) : "") + "/index/api/webrtc"
            : d_.public_base_url + d_.public_media_signaling_path;
        data["signaling"] = signaling + "?app=live&stream=" + stream + "&type=play";
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
            o["url"] = (d_.public_base_url.empty() ? "http://" + host : d_.public_base_url) +
                       "/recordings/" + basename_of(s.file_path);
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
            o["task_id"] = Json::Int64(e.task_id); o["rule_id"] = Json::Int64(e.rule_id);
            o["task_revision"] = Json::Int64(e.task_revision); o["rule_version"] = Json::Int64(e.rule_version);
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
        data["lifecycle_state"] = stopping_ ? "STOPPING" : data["ready"].asBool() ? "RUNNING" : "STARTING";
        const auto engine = d_.uds->state();
        data["engine_state"] = engine == UdsClient::State::ONLINE ? "ONLINE" : engine == UdsClient::State::SYNCING ? "SYNCING" : "DISCONNECTED";
        data["pending_operations"] = d_.db->pending_config_operations();
        data["engine_write_ready"] = data["ready"].asBool() && !stopping_ && cameras_->write_ready();
        data["sync_degraded_cams"] = d_.uds->sync_stats().degraded_cams;
        if (d_.ws) {
            const auto stats = d_.ws->stats();
            auto& websocket = data["websocket"];
            websocket["connections"] = static_cast<Json::UInt64>(d_.ws->conn_count());
            websocket["buffered_bytes"] = static_cast<Json::UInt64>(stats.buffered_bytes);
            websocket["buffered_frames"] = static_cast<Json::UInt64>(stats.buffered_frames);
            websocket["dropped_meta"] = static_cast<Json::UInt64>(stats.dropped_meta);
            websocket["slow_connections"] = static_cast<Json::UInt64>(stats.slow_connections);
            websocket["rejected_connections"] = static_cast<Json::UInt64>(stats.rejected_connections);
            websocket["invalid_input"] = static_cast<Json::UInt64>(stats.invalid_input);
        }
        data["uptime_s"] = static_cast<int>((steady_ms() - start_steady_ms_) / 1000);
        const auto cams = d_.db->list_cameras(true);
        int online = 0;
        for (const auto& c : cams)
            if (c.online) ++online;
        data["cameras_online"] = online;
        data["cameras_total"] = static_cast<int>(cams.size());
        data["ws_port"] = d_.ws_port;
        data["same_origin_ws"] = !d_.public_base_url.empty();
        data["public_base_url"] = d_.public_base_url;
        data["public_ws_url"] = d_.public_ws_url;
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
        auto lease = d_.uds->begin_configuration();
        if (!lease) { reply_err(res, 1003, "engine configuration is busy"); return; }
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
