// HttpApi —— REST 定义 14 条（T4.4 十二条 + M5 摄像头扫描/onvif_uri 两条）+ NPU 预检
//
// 统一响应包 {"code":0,"msg":"ok","data":...}；错误码详设 §8.2：
//   1001 参数 / 1002 不存在 / 1003 状态 / 3001 算力预检 / 3002 内存预检 /
//   4002 ZLM 失败 / 5001 内部；鉴权：首版无（局域网），配 auth_token 后校验
//   Authorization: Bearer <token>（401）。
// HTTP 框架：cpp-httplib（per-conn 线程，详设 §5.1"局域网少量并发足够"）；
// 业务错误一律 HTTP 200 + 包内 code（前端 api.js 按 code toast），
// 仅 401/404 用 HTTP 状态码。
//
// 预检（闸门1，详设 §5.4 细节）为纯函数 npu_precheck：按"去重物理模型"聚合
// Σ cost_ms×Σfps ÷ (核数×1000ms×80%) → compute_pct；Σ 去重 ctx mem ÷
// (总内存×80%) → mem_pct；任一 >100 → 3001/3002 + 建议文案。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>  // handle_put_* 签名直接用 Json::Value

#include "common/NonCopyable.h"
#include "db.h"
#include "resource_budget.h"

namespace httplib {
class Server;
class Request;
class Response;
}  // namespace httplib

namespace aibox {
namespace biz {

class UdsClient;
class ZlmClient;
class CameraService;
class WsServer;
class AccessSession;

// ---- HttpApi ----

class HttpApi : public NonCopyable {
public:
    struct Deps {
        Db* db = nullptr;
        UdsClient* uds = nullptr;  // SYNCING 未完成时命令返回 -1 → 透传 code=-1
        ZlmClient* zlm = nullptr;
        int http_port = 80;
        std::string bind_host = "0.0.0.0";
        std::string static_dir;      // web/ SPA（空 = 不托管）
        std::string recordings_dir;  // MP4 目录（空 = 不挂 /recordings）
        std::string auth_token;      // 空 = 不鉴权
        std::string public_base_url; // Explicit HTTPS origin for reverse-proxied access.
        std::string public_ws_url;   // Optional same-host WSS origin.
        std::string public_media_signaling_path = "/media/index/api/webrtc";
        std::shared_ptr<AccessSession> access;
        int ws_port = 8081;          // /api/system/info 下发（WsHub 独立端口）
        int npu_cores = 2;
        int64_t npu_mem_total_kb = 2097152;
        std::string version = "0.1.0";
        std::string media_host = "127.0.0.1";
        int media_rtsp_port = 554;
        int configuration_wait_ms = 1500;
        int configuration_retry_ms = 500;
        WsServer* ws = nullptr;
    };

    explicit HttpApi(Deps d);
    ~HttpApi();

    bool start();  // bind_to_port + listen 线程；端口占用返回 false
    void stop();
    void begin_shutdown();

    // §5.7-7：启动序列全部就绪后置位（/api/system/info 的 ready）
    void set_ready(bool r) { ready_.store(r, std::memory_order_relaxed); }
    int port() const { return port_; }

private:
    void register_routes();
    bool auth_ok(const httplib::Request& req) const;

    // PUT /api/cameras/{id} 与 /algos 两个 handler（板上 httplib 对路由做部分
    // 匹配且把 req.path 截断到匹配终点——分发依据=请求体结构，与路由引擎解耦；
    // camera body 恒无 "algos"，algos body 恒含 "algos" 数组，互斥完备）
    void handle_put_camera(const httplib::Request& req, httplib::Response& res,
                           int64_t id, const Json::Value& body);
    void handle_put_algos(const httplib::Request& req, httplib::Response& res,
                          int64_t id, const Json::Value& body);

    Deps d_;
    std::unique_ptr<CameraService> cameras_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread th_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stopping_{false};
    int64_t start_steady_ms_ = 0;
    int port_ = 0;
    std::mutex scan_mu_;
};

}  // namespace biz
}  // namespace aibox
