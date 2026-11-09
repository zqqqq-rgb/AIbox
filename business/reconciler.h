#pragma once

#include "config_compiler.h"
#include "uds_client.h"
#include "zlm_client.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace aibox {
namespace biz {

// Also called by UDS replay while holding its configuration lease.
bool reconcile_camera_media(Db& db, ZlmClient& zlm, const CameraConfigSnapshot& target,
                            const std::string& previous = "", std::string* error = nullptr);

class Reconciler : public NonCopyable {
public:
    struct Params {
        std::string media_host = "127.0.0.1";
        int media_port = 554;
        int retry_ms = 500;
        int poll_ms = 50;
        int media_check_ms = 5000;
    };
    Reconciler(Db& db, UdsClient& uds, ZlmClient& zlm, Params params);
    ~Reconciler();
    bool start();
    void stop();
    void request_stop();
    void wake();

private:
    void loop();
    int execute(const ConfigOperationRow& operation, UdsClient::ConfigurationLease& lease,
                std::string& error);
    Db& db_;
    UdsClient& uds_;
    ZlmClient& zlm_;
    Params params_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
} // namespace biz
} // namespace aibox
