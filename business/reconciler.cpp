#include "reconciler.h"
#include "common/Log.h"
#include <algorithm>
#include <chrono>

namespace aibox {
namespace biz {
namespace {
int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string proxy_key(int64_t id, bool main) {
    return "__defaultVhost__/live/cam" + std::to_string(id) + (main ? "_main" : "_sub");
}
} // namespace

bool reconcile_camera_media(Db& db, ZlmClient& zlm, const CameraConfigSnapshot& target,
                            const std::string& previous, std::string* error) {
    // Error text is public operation metadata; never include source URLs/credentials.
    if (error) *error = "media proxy update pending";
    CameraConfigState state;
    CameraRow current;
    const auto id = target.camera.id;
    if (!db.get_camera_config_state(id, state) || !db.get_camera(id, current))
        return false;
    CameraConfigSnapshot old;
    bool old_known = decode_camera_config(state.applied_config_json, old);
    if (!old_known)
        old_known = decode_camera_config(previous, old);
    if (!old_known && state.desired_revision == 0) {
        old = target;
        old_known = true;
    }
    const bool active = target.camera.enabled && !target.deleted;
    const std::string old_main =
        !state.media_main_url.empty()
            ? state.media_main_url
            : (old_known && old.camera.enabled && !current.zlm_key_main.empty()
                   ? old.camera.rtsp_main
                   : "");
    const std::string old_sub =
        !state.media_sub_url.empty()
            ? state.media_sub_url
            : (old_known && old.camera.enabled && !current.zlm_key_sub.empty() ? old.camera.rtsp_sub
                                                                               : "");

    auto replace = [&](bool main, const std::string& wanted, const std::string& known) {
        const auto key = proxy_key(id, main);
        if (wanted.empty() || wanted != known) {
            if (!zlm.del_proxy(key))
                return false;
            if (!db.set_camera_media(id, main, "", ""))
                return false;
        }
        if (wanted.empty())
            return true;
        const auto proxy = zlm.ensure_proxy(static_cast<int>(id), main, wanted);
        return proxy.err == ZlmClient::kOk && db.set_camera_media(id, main, wanted, proxy.key);
    };
    if (!active) {
        const bool main = replace(true, "", old_main);
        const bool sub = replace(false, "", old_sub);
        if (main && sub && error) error->clear();
        return main && sub;
    }
    const bool main_changed = old_main != target.camera.rtsp_main;
    if (!replace(true, target.camera.rtsp_main, old_main)) {
        if (main_changed && !old_main.empty() && !replace(true, old_main, "")) {
            LOGW("reconciler", "cam %lld main compensation pending", static_cast<long long>(id));
            if (error) *error = "main proxy replacement failed; compensation pending";
        }
        return false;
    }
    if (!replace(false, target.camera.rtsp_sub, old_sub)) {
        bool compensated = true;
        if (old_sub != target.camera.rtsp_sub && !old_sub.empty() && !replace(false, old_sub, "")) {
            compensated = false;
            LOGW("reconciler", "cam %lld substream compensation pending",
                 static_cast<long long>(id));
        }
        // Compensate only streams changed by this operation. Unchanged recording
        // proxies survive an algorithm/substream failure.
        if (main_changed && !replace(true, old_main, target.camera.rtsp_main)) {
            compensated = false;
            LOGW("reconciler", "cam %lld media compensation pending", static_cast<long long>(id));
        }
        if (error) *error = compensated ? "substream replacement failed; previous proxies restored"
                                       : "substream replacement failed; compensation pending";
        return false;
    }
    if (error) error->clear();
    return true;
}

Reconciler::Reconciler(Db& db, UdsClient& uds, ZlmClient& zlm, Params params)
    : db_(db), uds_(uds), zlm_(zlm), params_(std::move(params)) {}
Reconciler::~Reconciler() {
    stop();
}
bool Reconciler::start() {
    if (running_.exchange(true))
        return true;
    if (!db_.recover_config_operations()) {
        running_ = false;
        return false;
    }
    thread_ = std::thread([this] { loop(); });
    return true;
}
void Reconciler::stop() {
    request_stop();
    if (thread_.joinable())
        thread_.join();
}
void Reconciler::request_stop() {
    running_ = false;
    cv_.notify_all();
}
void Reconciler::wake() {
    cv_.notify_all();
}

int Reconciler::execute(const ConfigOperationRow& operation, UdsClient::ConfigurationLease& lease,
                        std::string& error) {
    CameraConfigSnapshot target, previous;
    CompiledCameraConfig compiled;
    if (!decode_camera_config(operation.target_config_json, target) ||
        !compile_camera_config(target, params_.media_host, params_.media_port, compiled, error)) {
        error = "stored configuration is invalid";
        return 1001;
    }
    if (target.camera.id != operation.camera_id) {
        error = "configuration camera mismatch";
        return 1001;
    }
    const bool had_previous = decode_camera_config(operation.previous_applied_json, previous);
    auto valid = [&] { return running_.load() && lease.valid(); };
    auto checked = [&](const char* step, const proto::Ack& ack) {
        if (!valid()) {
            error = "engine connection changed; awaiting replay";
            return -1;
        }
        if (ack.ret != 0)
            error = std::string(step) + " failed (" + std::to_string(ack.ret) + ")";
        return ack.ret;
    };
    const bool enabled = target.camera.enabled && !target.deleted;
    if (operation.op_type == "update" && had_previous) {
        auto renamed = previous;
        renamed.camera.name = target.camera.name;
        CameraConfigState state;
        if (db_.get_camera_config_state(operation.camera_id, state) &&
            !state.applied_config_json.empty() && valid() && operation.attempts == 1 &&
            operation.target_revision == state.applied_revision + 1 &&
            encode_camera_config(renamed) == operation.target_config_json && !uds_.supports_revision_ack())
            return 0;
    }
    const bool rebuild = !had_previous || !previous.camera.enabled ||
                         previous.camera.rtsp_main != target.camera.rtsp_main ||
                         previous.camera.rtsp_sub != target.camera.rtsp_sub;
    proto::RemoveCam remove;
    remove.id = static_cast<uint32_t>(operation.camera_id);
    if (!valid()) {
        error = "engine not ready";
        return -1;
    }
    if (!enabled || (rebuild && operation.op_type != "create")) {
        const auto ack = uds_.remove_cam(remove);
        if (!valid()) {
            error = "engine connection changed";
            return -1;
        }
        if (ack.ret != 0 && ack.ret != 1002)
            return checked("remove_cam", ack);
    }
    if (!reconcile_camera_media(db_, zlm_, target, operation.previous_applied_json, &error)) {
        return 4002;
    }
    if (!valid()) {
        error = "engine connection changed";
        return -1;
    }
    if (!enabled) {
        if (!db_.set_camera_online(operation.camera_id, false)) {
            error = "persist camera state failed";
            return 5001;
        }
        return 0;
    }
    auto add = [&]() {
        auto ack = uds_.add_cam(compiled.add);
        if (ack.ret == 1003 && valid()) {
            const auto removed = uds_.remove_cam(remove);
            if (!valid() || (removed.ret != 0 && removed.ret != 1002))
                return checked("remove_cam", removed);
            ack = uds_.add_cam(compiled.add);
        }
        return checked("add_cam", ack);
    };
    if (rebuild || operation.op_type == "create" || operation.attempts > 1) {
        const int rc = add();
        if (rc != 0)
            return rc;
    }
    if (operation.target_revision > 0)
        compiled.algos.configuration_revision = static_cast<uint64_t>(operation.target_revision);
    auto ack = uds_.set_algos(compiled.algos);
    if (ack.ret == 1002 && valid()) {
        const int rc = add();
        if (rc != 0)
            return rc;
        ack = uds_.set_algos(compiled.algos);
    }
    const auto result = checked("set_algos", ack);
    if (result == 0 && !db_.set_camera_online(operation.camera_id, true)) {
        error = "persist camera state failed";
        return 5001;
    }
    return result;
}

void Reconciler::loop() {
    auto next_media_check = wall_ms() + std::max(1, params_.media_check_ms);
    while (running_) {
        auto due = db_.due_config_operations(wall_ms());
        auto lease = due.empty() ? nullptr : uds_.begin_configuration();
        if (lease && running_ && db_.claim_config_operation(due.front().operation_id)) {
            auto operation = due.front();
            ++operation.attempts;
            std::string error;
            int code = 5001;
            try {
                code = execute(operation, *lease, error);
            } catch (const std::exception&) {
                error = "configuration execution failed; awaiting retry";
            }
            const bool permanent = code == 1001 || code == 1002 || code == 4001;
            const auto status = code == 0 ? "succeeded" : (permanent ? "failed" : "retry_wait");
            const auto retry =
                wall_ms() +
                std::min<int64_t>(30000, static_cast<int64_t>(std::max(1, params_.retry_ms)) *
                                             (1LL << std::min(operation.attempts - 1, 6)));
            if (!db_.finish_config_operation(operation, status, code, error,
                                             code == 0 ? 0 : retry)) {
                LOGE("reconciler", "operation result persistence failed: cam %lld",
                     static_cast<long long>(operation.camera_id));
                db_.recover_config_operations();
                // An unavailable confirmation store must not cause a tight apply/log loop.
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(std::max(1, params_.retry_ms)),
                             [&] { return !running_; });
            }
            lease.reset();
            cv_.notify_all();
            continue;
        }
        lease.reset();
        if (due.empty() && wall_ms() >= next_media_check) {
            auto media_lease = uds_.begin_configuration();
            if (media_lease) {
                for (const auto& cam : db_.list_cameras(true)) {
                    if (!running_ || !media_lease->valid())
                        break;
                    CameraConfigState state;
                    CameraConfigSnapshot target;
                    if (db_.get_camera_config_state(cam.id, state) &&
                        state.apply_status == "succeeded" &&
                        state.desired_revision == state.applied_revision &&
                        decode_camera_config(state.applied_config_json, target))
                        reconcile_camera_media(db_, zlm_, target);
                }
                next_media_check = wall_ms() + std::max(1, params_.media_check_ms);
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(std::max(1, params_.poll_ms)));
    }
}
} // namespace biz
} // namespace aibox
