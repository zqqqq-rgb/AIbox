// HookServer —— ZLM WebHook 接收端（任务分解 T4.5，详设 §4.2，:8090 仅 127.0.0.1）
//
// 端点（详设 §4.2 表）：
//   /hook/on_stream_changed  regist→camera online 字段 + WS 徽标（经 hooks 上抛）
//   /hook/on_record_mp4      file/start_time/time_len → segments 插入（hooks 上抛）
//   /hook/on_flow_report     流量统计（V1 忽略，回 code 0）
//   其余 /hook/*             回 code 0（防 ZLM 无谓重试），LOGI 记录
// 响应统一 {"code":0,"msg":"success"}（详设 §8.3）。
// on_publish/on_play 鉴权 hook 暂不启用（详设 §4.2 注）。
//
// 参数格式兼容：ZLM hook 既有 form 也有 json 提交（版本相关）——按 body 首字符
// '{' 判别，form 用 urlencoded 解析。hook 处理绝不做重活（快回），副作用经
// hooks 回调到 main 的分发线程执行。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "common/NonCopyable.h"
#include "db.h"

namespace httplib {
class Server;
}

namespace aibox {
namespace biz {

class HookServer : public NonCopyable {
public:
    struct Hooks {
        // regist=true/false（stream 形如 camN_main/camN_sub → cam id 解析在内部，
        // 未按 camN 命名的流 id 传 -1）
        std::function<void(int64_t cam, bool online)> on_stream_changed;
        std::function<void(const SegmentRow&)> on_record_mp4;
    };

    struct Params {
        std::string bind_host = "127.0.0.1";  // 仅本机（ZLM 与 businessd 同机）
        int port = 8090;
    };

    // 默认参数用重载代替（嵌套类聚合默认实参 = {} GCC 拒绝，同 CameraChannel 先例）
    HookServer(Hooks hooks);
    HookServer(Hooks hooks, Params p);
    ~HookServer();

    bool start();
    void stop();

private:
    void register_routes();

    Hooks hooks_;
    Params p_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread th_;  // 由 NonCopyable 成员序隐式可用——显式声明避免遗漏
};

}  // namespace biz
}  // namespace aibox
