// FakeEngine —— 复用 aibox_core UdsServer 的 mock engine（T4.2 测试支撑，任务卡
// "~150 行 mock 进程按脚本回 ack/事件"的进程内等价物：同 socket 语义、同协议栈）
// 脚本：script.fail_add_cam / fail_set_algos 指定 cam 恒失败；计数器供断言
// （UdsClient 重试×2 → engine 侧共收到 3 次同 cmd）
#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ipc/Protocol.h"
#include "ipc/UdsServer.h"

namespace aibox {
namespace testutil {

class FakeEngine {
public:
    struct Script {
        std::set<uint32_t> fail_add_cam;    // add_cam ack ret=1003
        std::set<uint32_t> fail_set_algos;  // set_algos ack ret=1003
    };

    FakeEngine() : uds_(0 /*禁用空闲心跳：测试不喂狗*/) { register_cmds(); }
    ~FakeEngine() { stop(); }

    bool start(const std::string& path) { return uds_.start(path); }
    void stop() { uds_.stop(); }  // 关闭传输；新实例模拟 engine 进程丢失内存状态
    void emit(const std::string& evt_line) { uds_.emit(evt_line, 0); }

    bool camera(uint32_t id, proto::AddCam& out, proto::SetAlgos& algos) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cameras_.find(id);
        if (it == cameras_.end()) return false;
        out = it->second;
        auto a = algos_.find(id);
        algos = a == algos_.end() ? proto::SetAlgos{} : a->second;
        return true;
    }
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mu_);
        return commands_;
    }

    Script script;
    std::atomic<int> add_cam{0};
    std::atomic<int> set_algos{0};
    std::atomic<int> remove_cam{0};
    std::atomic<int> reload{0};
    std::atomic<int> get_metrics{0};

private:
    void register_cmds() {
        uds_.on_cmd("add_cam", [this](const proto::ParsedMsg& m) {
            std::lock_guard<std::mutex> lock(mu_);
            commands_.push_back("add_cam:" + std::to_string(m.add_cam.id));
            ++add_cam;
            proto::Ack a;
            if (script.fail_add_cam.count(m.add_cam.id)) {
                a.ret = 1003;
                a.msg = "fake fail";
            } else if (cameras_.count(m.add_cam.id)) {
                a.ret = 1003;
                a.msg = "cam already exists";
            } else {
                cameras_[m.add_cam.id] = m.add_cam;
            }
            return a;
        });
        uds_.on_cmd("set_algos", [this](const proto::ParsedMsg& m) {
            std::lock_guard<std::mutex> lock(mu_);
            commands_.push_back("set_algos:" + std::to_string(m.set_algos.id));
            ++set_algos;
            proto::Ack a;
            if (script.fail_set_algos.count(m.set_algos.id)) {
                a.ret = 1003;
                a.msg = "fake fail";
            } else if (!cameras_.count(m.set_algos.id)) {
                a.ret = 1002;
                a.msg = "cam not found";
            } else {
                algos_[m.set_algos.id] = m.set_algos;
            }
            return a;
        });
        uds_.on_cmd("remove_cam", [this](const proto::ParsedMsg& m) {
            std::lock_guard<std::mutex> lock(mu_);
            commands_.push_back("remove_cam:" + std::to_string(m.remove_cam.id));
            ++remove_cam;
            proto::Ack ack;
            if (!cameras_.erase(m.remove_cam.id)) ack.ret = 1002;
            algos_.erase(m.remove_cam.id);
            return ack;
        });
        uds_.on_cmd("reload_algo", [this](const proto::ParsedMsg&) {
            ++reload;
            return proto::Ack{};
        });
        uds_.on_cmd("get_metrics", [this](const proto::ParsedMsg&) {
            ++get_metrics;
            proto::Ack a;
            a.has_data = true;
            a.data = "{}";
            return a;
        });
    }

    UdsServer uds_;
    mutable std::mutex mu_;
    std::map<uint32_t, proto::AddCam> cameras_;
    std::map<uint32_t, proto::SetAlgos> algos_;
    std::vector<std::string> commands_;
};

}  // namespace testutil
}  // namespace aibox
