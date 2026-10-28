// ZlmClient —— ZLM(MediaServer) REST 封装 + 流编排事务（任务分解 T4.3，详设 §4.1/§5.3）
//
// 单写者纪律：全部 ZLM REST 写操作仅 businessd 发起（架构 §3.1）
// API 面（详设 §4.1）：
//   POST /index/api/addStreamProxy   拉代理（enable_mp4 主1子0 / retry_count=-1 / rtp_type=0）
//   POST /index/api/delStreamProxy   删代理（幂等）
//   GET  /index/api/getMediaList     代理健康（Monitor 轮询 / SYNCING 前对比 DB）
//   GET  /index/api/getServerConfig  启动自检（防产线配置漂移）
//
// 编排事务（详设 §5.3）：add_main + add_sub 均成功才算建好；任一失败 →
// 回滚 del 已建代理（防半配置）→ 报错。幂等：先 getMediaList 查存在性，
// 已存在按成功返回既有 key（同名重添不产生双代理）。
//
// HTTP 传输：自写极简客户端（HTTP/1.1 + Connection: close + Content-Length）。
// ZLM REST 交互面窄且固定（上列 4 个 API），form 表单 + JSON 应答，
// 自写 ~150 行可控可测；不引入完整 HTTP 客户端依赖。
// 错误码：ZLM code 原样透传；传输层失败统一 4002（详设 §8.2 ZLM REST 失败）。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <json/json.h>

#include "common/NonCopyable.h"

namespace aibox {
namespace biz {

class ZlmClient : public NonCopyable {
public:
    constexpr static int kOk = 0;
    constexpr static int kErrTransport = 4002;

    struct Params {
        std::string host = "127.0.0.1";
        int port = 80;              // ZLM HTTP 端口
        std::string secret;         // config.ini 的 api secret
        int timeout_ms = 3000;      // 详设 §4.1：超时 3s
    };

    explicit ZlmClient(Params p);

    // WebRTC 信令 URL 拼装需要 ZLM HTTP 端口（http_api stream 路由用）
    int port() const { return p_.port; }

    struct ProxyResult {
        bool created = false;  // 本次调用新建；回滚不得删除复用的代理
        std::string key;            // 成功时代理 key（delStreamProxy 用）
        int err = kOk;              // ZLM code / kErrTransport
        std::string msg;
    };

    // —— 单代理操作（幂等语义在 ensure 上）——
    // app 固定 "live"，stream = cam{id}_main / cam{id}_sub（架构 §4.3 流命名规范）
    // main=true: enable_mp4=1（录像源）；false: enable_mp4=0（AI 源）
    ProxyResult ensure_proxy(int cam, bool main, const std::string& rtsp);
    // best-effort 删除：key 不存在视为已删（返回 true）；传输失败返回 false
    bool del_proxy(const std::string& key);

    // —— 编排事务（摄像头 API 层调用）——
    // 均成功 → key_main/key_sub 回填、返回 true；任一失败 → 回滚已建代理 → false
    bool add_camera_proxies(int cam, const std::string& rtsp_main,
                            const std::string& rtsp_sub, std::string* key_main,
                            std::string* key_sub);

    // —— 只读 ——
    bool media_list(Json::Value* out);         // data 为数组（app/stream/... 条目）
    bool get_server_config(Json::Value* out);  // data 为对象（启动自检比对）

    // —— 截图（ROI 编辑底图，事件快照 V1 未接）——
    // getSnap 单帧 JPEG；url = 任一 ZLM 可拉取的流地址（调用方传摄像头 rtsp_sub）。
    // 成功 true 且 jpeg 为完整 JPEG（0xFFD8 开头）；失败 false（离线/超时/错误 JSON）。
    // timeout_sec：getSnap 侧首帧拉流等待；HTTP 层超时 = timeout_sec + 2s 兜底
    bool get_snap(const std::string& url, int timeout_sec, std::string* jpeg);

private:
    // —— 无锁内部实现（公共方法持 mu_ 后调用；impl 之间互调不死锁）——
    // timeout_ms：0 = 用 p_.timeout_ms（get_snap 传长超时覆盖）
    bool http(const char* method, const std::string& path,
              const std::string& encoded_form, std::string* body_out,
              int timeout_ms = 0);
    bool call_api(const char* method, const std::string& path,
                  const std::vector<std::pair<std::string, std::string>>& form,
                  int* code, std::string* msg, Json::Value* data);
    bool media_list_impl(Json::Value* out);
    bool find_proxy_key_impl(const std::string& app, const std::string& stream,
                             std::string* key);
    ProxyResult ensure_proxy_impl(int cam, bool main, const std::string& rtsp);
    bool del_proxy_impl(const std::string& key);

    Params p_;
    std::mutex mu_;  // 公共方法薄锁壳（单写者纪律的进程内体现）
};

}  // namespace biz
}  // namespace aibox
