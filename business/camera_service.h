#pragma once

#include "reconciler.h"
#include <atomic>
#include <optional>

namespace aibox {
namespace biz {
struct CameraPatch {
    std::optional<std::string> name, rtsp_main, rtsp_sub;
    std::optional<bool> enabled;
};
struct ConfigurationResult {
    int code = 0;
    std::string message, operation_id, apply_status;
    int64_t camera_id = 0, revision = 0;
    bool accepted = false;
    double compute_pct = 0, mem_pct = 0;
};
struct TaskConfigurationResult {
    int code = 0;
    std::string message;
    bool accepted = false;
    int64_t revision = 0;
    std::vector<ConfigurationResult> cameras;
};

class CameraService : public NonCopyable {
public:
    struct Params {
        Reconciler::Params reconciler;
        int npu_cores = 2;
        int64_t npu_mem_kb = 2097152;
        int apply_wait_ms = 1500;
        int schedule_poll_ms = 1000;
        std::function<int64_t()> schedule_now = {};
    };
    CameraService(Db& db, UdsClient& uds, ZlmClient& zlm, Params params);
    ~CameraService();
    bool start();
    void begin_shutdown(); // Close admission and request executor stop without joining.
    bool write_ready();
    void stop();
    ConfigurationResult create(const CameraRow& target);
    ConfigurationResult update(int64_t id, const CameraPatch& patch);
    ConfigurationResult set_algos(int64_t id, const std::vector<CamAlgoRow>& algos);
    ConfigurationResult remove(int64_t id);
    TaskConfigurationResult set_task_enabled(int64_t id, int64_t revision, bool enabled);
    bool runtime_online() const {
        return running_ && uds_.online();
    }
    int64_t current_time() const;
    bool reconcile_schedules();

private:
    ConfigurationResult submit(int64_t id, const CameraRow* created, const CameraPatch* patch,
                               const std::vector<CamAlgoRow>* algos, bool deleted);
    ConfigurationResult wait(ConfigurationResult result);
    bool budget(const CameraRow& target, const std::vector<CamAlgoRow>& algos,
                ConfigurationResult& result, const std::vector<AlgorithmTask>* tasks = nullptr);
    Db& db_;
    UdsClient& uds_;
    Params params_;
    Reconciler reconciler_;
    std::atomic<bool> running_{false};
    std::mutex admission_mu_;
    std::thread schedule_thread_;
    std::mutex schedule_mu_;
    std::condition_variable schedule_cv_;
};
} // namespace biz
} // namespace aibox
