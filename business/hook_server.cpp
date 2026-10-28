#include "hook_server.h"
#include "http_compat.h"

#include <httplib.h>

#include <json/json.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "common/Log.h"

namespace aibox {
namespace biz {

namespace {

std::string ok_body() { return "{\"code\":0,\"msg\":\"success\"}"; }

void reply_ok(httplib::Response& res) { res.set_content(ok_body(), "application/json"); }

// stream "camN_main"/"camN_sub" → N；非 camN 命名 → -1
int64_t cam_of_stream(const std::string& stream) {
    if (stream.rfind("cam", 0) != 0) return -1;
    const size_t underscore = stream.find('_');
    if (underscore == std::string::npos) return -1;
    return std::atoll(stream.substr(3, underscore - 3).c_str());
}

auto percent_decode = [](const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o.push_back(static_cast<char>(
                std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            o.push_back(' ');
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
};

// hook 参数：json 或 urlencoded 二选一（按 body 首字符判别）→ 统一键值表
std::map<std::string, std::string> hook_params(const std::string& body) {
    std::map<std::string, std::string> out;
    if (!body.empty() && body.front() == '{') {
        Json::Value root;
        Json::CharReaderBuilder b;
        std::unique_ptr<Json::CharReader> rd(b.newCharReader());
        std::string errs;
        if (rd->parse(body.data(), body.data() + body.size(), &root, &errs)) {
            for (const auto& k : root.getMemberNames()) {
                const Json::Value& v = root[k];
                if (v.isString())
                    out[k] = v.asString();
                else if (v.isNumeric())
                    out[k] = std::to_string(v.asDouble());
                else if (v.isBool())
                    out[k] = v.asBool() ? "1" : "0";
            }
        }
        return out;
    }
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t amp = body.find('&', pos);
        const std::string pair =
            body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[percent_decode(pair.substr(0, eq))] =
                percent_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

}  // namespace

HookServer::HookServer(Hooks hooks) : HookServer(std::move(hooks), Params{}) {}

HookServer::HookServer(Hooks hooks, Params p) : hooks_(std::move(hooks)), p_(p) {}

HookServer::~HookServer() { stop(); }

bool HookServer::start() {
    svr_ = std::make_unique<httplib::Server>();
    register_routes();
    if (!svr_->bind_to_port(p_.bind_host.c_str(), p_.port)) {
        LOGE("hook", "bind %s:%d 失败", p_.bind_host.c_str(), p_.port);
        svr_.reset();
        return false;
    }
    th_ = std::thread([this] { svr_->listen_after_bind(); });
    LOGI("hook", "HookServer listening %s:%d", p_.bind_host.c_str(), p_.port);
    return true;
}

void HookServer::stop() {
    if (svr_) svr_->stop();
    if (th_.joinable()) th_.join();
    svr_.reset();
}

void HookServer::register_routes() {
    // 同时兼容板端 exception& 与发行版 exception_ptr 回调签名。
    svr_->set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, auto&& ep) {
            LOGE("hook", "handler 异常: %s", http_exception_message(ep).c_str());
            reply_ok(res);  // hook 面向 ZLM：异常也回 code 0，防重试风暴（内部已记日志）
        });

    svr_->Post("/hook/on_stream_changed",
               [this](const httplib::Request& req, httplib::Response& res) {
        const auto params = hook_params(req.body);
        // const map 不可 operator[]：统一走 find helper
        auto get = [&params](const char* k) -> std::string {
            const auto it = params.find(k);
            return it == params.end() ? std::string() : it->second;
        };
        const std::string stream = get("stream");
        const std::string regist_s = get("regist");
        const bool regist = !regist_s.empty() && regist_s != "0" && regist_s != "false";
        const int64_t cam = cam_of_stream(stream);
        LOGI("hook", "on_stream_changed %s regist=%d", stream.c_str(), regist ? 1 : 0);
        if (cam > 0 && hooks_.on_stream_changed) hooks_.on_stream_changed(cam, regist);
        reply_ok(res);
    });

    svr_->Post("/hook/on_record_mp4",
               [this](const httplib::Request& req, httplib::Response& res) {
        const auto params = hook_params(req.body);
        auto get = [&params](const char* k) -> std::string {
            const auto it = params.find(k);
            return it == params.end() ? std::string() : it->second;
        };
        SegmentRow seg;
        seg.camera_id = cam_of_stream(get("stream"));
        seg.file_path = get("file");
        // ZLM：start_time = unix 秒（可浮点），time_len = 秒（可浮点）
        seg.start_ts = static_cast<int64_t>(std::atof(get("start_time").c_str()) * 1000.0);
        seg.duration_s = std::atof(get("time_len").c_str());
        seg.end_ts = seg.start_ts + static_cast<int64_t>(seg.duration_s * 1000.0);
        if (seg.camera_id > 0 && !seg.file_path.empty() && hooks_.on_record_mp4) {
            hooks_.on_record_mp4(seg);
        } else {
            LOGW("hook", "on_record_mp4 参数不全: stream=%s file=%s", get("stream").c_str(),
                 seg.file_path.c_str());
        }
        reply_ok(res);
    });

    svr_->Post("/hook/on_flow_report",
               [](const httplib::Request&, httplib::Response& res) { reply_ok(res); });

    // 兜底：未实现 hook 回 code 0（防 ZLM 重试风暴），LOGI 留痕
    svr_->Post(R"(/hook/.*)",
               [](const httplib::Request& req, httplib::Response& res) {
        LOGI("hook", "未实现 hook（回 code 0）: %s", req.path.c_str());
        reply_ok(res);
    });
}

}  // namespace biz
}  // namespace aibox
