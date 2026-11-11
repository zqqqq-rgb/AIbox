#include "camera_service.h"
#include "resource_budget.h"
#include "task_compiler.h"
#include <algorithm>
#include <chrono>
#include <json/json.h>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>

namespace aibox {
namespace biz {
EngineObservation CameraService::observe_engine() {
    if (!running_) return {};
    if (uds_.state() == UdsClient::State::SYNCING) return {"engine_syncing", {}};
    if (!uds_.online()) return {"engine_offline", {}};
    const auto epoch = uds_.connection_epoch();
    // Old peers can close the connection on unknown commands. Probe only after advertisement.
    if (!uds_.supports_revision_ack()) return {"engine_declaration_missing", {}};
    std::string text;
    if (!uds_.get_capabilities(&text, 300, true) || !runtime_online() || uds_.connection_epoch() != epoch)
        return {"engine_query_failed", {}};
    Json::CharReaderBuilder builder;
    builder["failIfExtra"] = true;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    Json::Value capabilities;
    std::string error;
    if (!reader->parse(text.data(), text.data() + text.size(), &capabilities, &error) ||
        !capabilities.isObject())
        return {"engine_query_failed", {}};
    return {"online", std::move(capabilities)};
}

namespace {
std::string operation_id() {
    static std::atomic<uint64_t> sequence{0};
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "cfg-" + std::to_string(ns) + "-" + std::to_string(::getpid()) + "-" +
           std::to_string(++sequence);
}
ConfigurationResult reject(int code, const std::string& message) {
    ConfigurationResult result;
    result.code = code;
    result.message = message;
    return result;
}
std::string ordered_snapshot(CameraConfigSnapshot snapshot) {
    std::stable_sort(snapshot.algos.begin(), snapshot.algos.end(),
                     [](const CamAlgoRow& left, const CamAlgoRow& right) {
                         return left.algo_id < right.algo_id;
                     });
    return encode_camera_config(snapshot);
}
} // namespace

CameraService::CameraService(Db& db, UdsClient& uds, ZlmClient& zlm, Params params)
    : db_(db), uds_(uds), params_(std::move(params)),
      reconciler_(db, uds, zlm, params_.reconciler) {}
CameraService::~CameraService() {
    stop();
}
bool CameraService::start() {
    if (running_) return true;
    running_ = reconciler_.start();
    if (running_ && params_.schedule_poll_ms > 0)
        schedule_thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(schedule_mu_);
            while (running_) {
                lock.unlock();
                reconcile_schedules();
                lock.lock();
                schedule_cv_.wait_for(lock, std::chrono::milliseconds(params_.schedule_poll_ms),
                                      [this] { return !running_; });
            }
        });
    return running_;
}
void CameraService::stop() {
    begin_shutdown();
    if (schedule_thread_.joinable())
        schedule_thread_.join();
    reconciler_.stop();
}
void CameraService::begin_shutdown() {
    running_ = false;
    schedule_cv_.notify_all();
    // Any admission already in progress must finish before shutdown proceeds to UDS stop.
    std::lock_guard<std::mutex> lock(admission_mu_);
    reconciler_.request_stop();
}
bool CameraService::write_ready() {
    if (!running_)
        return false;
    auto lease = uds_.begin_configuration();
    return running_ && static_cast<bool>(lease);
}
int64_t CameraService::current_time() const {
    return params_.schedule_now ? params_.schedule_now() : task_now_ms();
}
bool CameraService::reconcile_schedules() {
    std::lock_guard<std::mutex> admission(admission_mu_);
    if (!running_)
        return false;
    auto lease = uds_.begin_configuration();
    if (!lease || !db_.refresh_task_schedules(current_time()))
        return false;
    reconciler_.wake();
    return true;
}
ConfigurationResult CameraService::create(const CameraRow& target) {
    return submit(0, &target, nullptr, nullptr, false);
}
ConfigurationResult CameraService::update(int64_t id, const CameraPatch& patch) {
    return submit(id, nullptr, &patch, nullptr, false);
}
ConfigurationResult CameraService::set_algos(int64_t id, const std::vector<CamAlgoRow>& algos) {
    return submit(id, nullptr, nullptr, &algos, false);
}
ConfigurationResult CameraService::remove(int64_t id) {
    return submit(id, nullptr, nullptr, nullptr, true);
}

TaskConfigurationResult CameraService::set_task_enabled(int64_t id, int64_t revision,
                                                        bool enabled) {
    TaskConfigurationResult result;
    auto fail = [&](int code, const std::string& message) {
        result.code = code;
        result.message = message;
        return result;
    };
    std::unique_lock<std::mutex> admission(admission_mu_);
    if (!running_)
        return fail(1003, "configuration service is stopping");
    auto lease = uds_.begin_configuration();
    if (!lease)
        return fail(1003, "engine is disconnected, synchronizing, or applying configuration");
    AlgorithmTask task;
    if (!db_.get_task(id, task) || task.deleted_at)
        return fail(1002, "task not found");
    if (task.kind != "v2")
        return fail(1003, "legacy task lifecycle follows its camera");
    if (revision <= 0 || revision == INT64_MAX || revision != task.revision)
        return fail(1003, "task revision conflict");
    const auto at = current_time();
    std::vector<ConfigOperationRow> operations;
    if (task.enabled != enabled) {
        if (enabled) {
            std::string caps;
            if (!uds_.get_capabilities(&caps, 300) || !proto::supports_rule_v2(caps))
                return fail(1003, "engine does not advertise V2 rule support");
        }
        auto tasks = db_.list_tasks();
        for (auto& value : tasks)
            if (value.id == id) {
                value.enabled = enabled;
                ++value.revision;
            }
        CameraRow focus;
        bool have_focus = false;
        for (auto cam : task.camera_ids) {
            CameraRow camera;
            CameraConfigState state;
            CameraConfigSnapshot target;
            std::string error;
            if (!db_.get_camera(cam, camera) || !db_.get_camera_config_state(cam, state) ||
                (enabled && state.deleted_at))
                return fail(1002, "task references a missing or deleted camera");
            if (!build_camera_config(db_, camera, db_.list_camera_algos(cam), state.deleted_at != 0,
                                     target, error, &tasks, at))
                return fail(1001, error);
            if (!have_focus) {
                focus = camera;
                have_focus = true;
            }
            ConfigOperationRow operation;
            operation.operation_id = operation_id();
            operations.push_back(operation);
        }
        if (!have_focus)
            return fail(1001, "task has no cameras");
        ConfigurationResult estimate;
        if (enabled && !budget(focus, db_.list_camera_algos(focus.id), estimate, &tasks))
            return fail(estimate.code, estimate.message);
        if (!running_ || !lease->valid())
            return fail(1003, "engine connection changed before admission");
        if (!db_.submit_task_enabled(id, revision, enabled, operations, at)) {
            AlgorithmTask latest;
            if (db_.get_task(id, latest) && latest.revision != revision)
                return fail(1003, "task revision conflict");
            return fail(5001, "task and camera operations were not committed");
        }
        result.revision = revision + 1;
    } else {
        result.revision = revision;
        for (const auto& deployment : db_.task_deployments(id)) {
            ConfigOperationRow operation;
            if (!db_.get_current_config_operation(deployment.camera_id, operation))
                return fail(5001, "task deployment state is unavailable");
            operations.push_back(std::move(operation));
        }
    }
    result.accepted = true;
    for (const auto& operation : operations) {
        ConfigurationResult camera;
        camera.camera_id = operation.camera_id;
        camera.operation_id = operation.operation_id;
        camera.revision = operation.target_revision;
        camera.accepted = true;
        result.cameras.push_back(camera);
    }
    lease.reset();
    admission.unlock();
    reconciler_.wake();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, params_.apply_wait_ms));
    for (;;) {
        bool applied = true, terminal = false;
        for (auto& camera : result.cameras) {
            ConfigOperationRow operation;
            if (!db_.get_config_operation(camera.operation_id, operation)) {
                camera.apply_status = "unknown";
                applied = false;
                continue;
            }
            camera.apply_status = operation.status;
            camera.code = operation.error_code;
            applied = applied && operation.status == "succeeded";
            terminal = terminal || operation.status == "failed" || operation.status == "superseded";
        }
        if (applied && runtime_online()) {
            result.code = 0;
            result.message = "task target applied";
            return result;
        }
        if (terminal || !running_ || std::chrono::steady_clock::now() >= deadline) {
            result.code = 1003;
            result.message = "task target saved; inspect camera operations for application status";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool CameraService::budget(const CameraRow& target, const std::vector<CamAlgoRow>& algos,
                           ConfigurationResult& result, const std::vector<AlgorithmTask>* tasks) {
    // Reserve peak demand even when a scheduled window is currently inactive.
    auto reservations = tasks ? *tasks : db_.list_tasks();
    for (auto& task : reservations)
        task.schedules.clear();
    tasks = &reservations;
    PrecheckInput input;
    input.focus_cam = static_cast<int>(target.id);
    input.npu_cores = params_.npu_cores;
    input.npu_mem_total_kb = params_.npu_mem_kb;
    for (const auto& m : db_.list_algorithms_meta())
        if (!m.disabled)
            input.models.push_back(
                {m.algo_id, m.npu_cost_ms, m.npu_mem_kb, m.trigger_type == "triggered"});
    std::map<std::pair<int64_t, std::string>, int> reserved;
    bool strict_v2 = false;
    auto include = [&](const CameraConfigSnapshot& snapshot) {
        if (!snapshot.camera.enabled || snapshot.deleted)
            return;
        if (snapshot.has_v2) {
            strict_v2 = true;
            for (const auto& a : snapshot.runtime.algos) {
                auto& fps = reserved[{snapshot.camera.id, a.algo}];
                fps = std::max(fps, int(a.fps));
            }
            return;
        }
        for (const auto& a : snapshot.algos)
            if (a.enabled) {
                auto& fps = reserved[{snapshot.camera.id, a.algo_id}];
                fps = std::max(fps, a.detect_fps);
            }
    };
    for (const auto& cam : db_.list_cameras(false, true)) {
        CameraConfigState state;
        if (!db_.get_camera_config_state(cam.id, state)) {
            result = reject(5001, "read configuration state failed");
            return false;
        }
        CameraConfigSnapshot desired;
        std::string error;
        if (!build_camera_config(db_, cam.id == target.id ? target : cam,
                                 cam.id == target.id ? algos : db_.list_camera_algos(cam.id),
                                 state.deleted_at != 0, desired, error, tasks)) {
            result = reject(1001, error);
            return false;
        }
        include(desired);
        auto snapshots = db_.reserved_config_snapshots(cam.id);
        if (!state.applied_config_json.empty())
            snapshots.push_back(state.applied_config_json);
        for (const auto& json : snapshots) {
            if (json.empty())
                continue;
            CameraConfigSnapshot previous;
            if (!decode_camera_config(json, previous)) {
                result = reject(1003, "unconfirmed resource reservation requires recovery");
                return false;
            }
            include(previous);
        }
    }
    include({target, algos, false});
    for (const auto& item : reserved)
        input.usages.push_back(
            {item.first.second, static_cast<int>(item.first.first), item.second});
    if (!input.usages.empty()) {
        std::string metrics;
        if (!uds_.get_metrics(&metrics, 300)) {
            result = reject(1003, "engine resource plan is unavailable");
            return false;
        }
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> parser(reader.newCharReader());
        std::string errors;
        if (!parser->parse(metrics.data(), metrics.data() + metrics.size(), &root, &errors) ||
            !root.isObject()) {
            result = reject(1003, "invalid engine resource plan");
            return false;
        }
        const auto& npu = root["npu"];
        if (strict_v2 && (!npu.isObject() || !npu["steady_cores"].isArray() ||
                          !npu["triggered_cores"].isArray())) {
            result = reject(1003, "V2 requires an explicit engine core assignment");
            return false;
        }
        if (strict_v2) {
            std::set<unsigned> all;
            for (const auto* name : {"steady_cores", "triggered_cores"})
                for (const auto& core : npu[name])
                    if (!core.isUInt() || core.asUInt() > 2 || !all.insert(core.asUInt()).second) {
                        result = reject(1003, "invalid engine core assignment");
                        return false;
                    }
        }
        if (npu.isObject() && npu["steady_cores"].isArray()) {
            std::set<unsigned> cores;
            for (const auto& core : npu["steady_cores"]) {
                if (!core.isUInt() || core.asUInt() > 2 || !cores.insert(core.asUInt()).second) {
                    result = reject(1003, "invalid engine core assignment");
                    return false;
                }
            }
            input.npu_cores = std::min(input.npu_cores, static_cast<int>(cores.size()));
        }
        if (npu.isObject() && npu["triggered_cores"].isArray() && npu["triggered_cores"].empty()) {
            for (const auto& usage : input.usages)
                for (const auto& model : input.models)
                    if (usage.algo_id == model.algo_id && model.triggered) {
                        result = reject(1003, "engine has no triggered worker");
                        return false;
                    }
        }
    }
    const auto checked = npu_precheck(input);
    result.compute_pct = checked.compute_pct;
    result.mem_pct = checked.mem_pct;
    if (!checked.ok) {
        result.code = checked.code;
        result.message = checked.msg;
    }
    return checked.ok;
}

ConfigurationResult CameraService::submit(int64_t id, const CameraRow* created,
                                          const CameraPatch* patch,
                                          const std::vector<CamAlgoRow>* algos, bool deleted) {
    std::unique_lock<std::mutex> admission(admission_mu_);
    if (!running_)
        return reject(1003, "configuration service is stopping");
    auto lease = uds_.begin_configuration();
    if (!lease)
        return reject(1003, "engine is disconnected, synchronizing, or applying configuration");
    CameraRow target, previous;
    CameraConfigState state;
    std::vector<CamAlgoRow> rows, old_rows;
    if (created) {
        target = *created;
        target.id = 0;
    } else {
        if (!db_.get_camera(id, target) || !db_.get_camera_config_state(id, state) ||
            (state.deleted_at && !deleted))
            return reject(1002, "camera not found");
        previous = target;
        rows = old_rows = db_.list_camera_algos(id);
    }
    if (patch) {
        if (patch->name)
            target.name = *patch->name;
        if (patch->rtsp_main)
            target.rtsp_main = *patch->rtsp_main;
        if (patch->rtsp_sub)
            target.rtsp_sub = *patch->rtsp_sub;
        if (patch->enabled)
            target.enabled = *patch->enabled;
    }
    if (algos)
        rows = *algos;
    if (deleted)
        target.enabled = false;
    CompiledCameraConfig compiled;
    std::string error;
    CameraConfigSnapshot full;
    if (algos && !compile_algos(static_cast<uint32_t>(target.id), rows, compiled.algos, error))
        return reject(1001, error);
    if (!build_camera_config(db_, target, rows, deleted, full, error) ||
        !compile_camera_config(full, params_.reconciler.media_host, params_.reconciler.media_port,
                               compiled, error))
        return reject(1001, error);
    if (!created) {
        ConfigOperationRow current;
        CameraConfigSnapshot saved;
        if (db_.get_current_config_operation(id, current) &&
            (current.status == "pending" || current.status == "running" ||
             current.status == "retry_wait") &&
            decode_camera_config(current.target_config_json, saved) &&
            ordered_snapshot(full) == ordered_snapshot(saved)) {
            ConfigurationResult reused;
            reused.accepted = true;
            reused.camera_id = id;
            reused.operation_id = current.operation_id;
            reused.revision = current.target_revision;
            lease.reset();
            admission.unlock();
            return wait(std::move(reused));
        }
        if (state.deleted_at)
            return reject(1002, "camera not found");
    }
    // Disabled algorithm definitions remain editable, but cannot enter the runtime budget.
    std::map<std::string, int> old_fps;
    CameraConfigSnapshot before;
    CompiledCameraConfig old_compiled;
    if (previous.enabled && build_camera_config(db_, previous, old_rows, false, before, error) &&
        compile_camera_config(before, params_.reconciler.media_host, params_.reconciler.media_port,
                              old_compiled, error))
        for (const auto& a : old_compiled.algos.algos)
            old_fps[a.algo] = a.fps;
    bool increases = false;
    if (target.enabled)
        for (const auto& a : compiled.algos.algos)
            if (a.fps > old_fps[a.algo])
                increases = true;
    ConfigurationResult result;
    if (target.enabled && !budget(target, rows, result) && increases)
        return result;
    result.code = 0;
    result.message.clear();
    ConfigOperationRow operation;
    operation.operation_id = operation_id();
    operation.op_type =
        deleted   ? "delete"
        : created ? "create"
        : algos   ? "set_algos"
                  : (previous.enabled != target.enabled ? (target.enabled ? "enable" : "disable")
                                                        : "update");
    operation.previous_applied_json = state.applied_config_json;
    if (operation.previous_applied_json.empty() && !created) {
        // Preserve the earliest unconfirmed runtime baseline across superseding submissions.
        auto reserved = db_.reserved_config_snapshots(id);
        for (size_t i = 1; i < reserved.size(); i += 2)
            if (!reserved[i].empty()) {
                operation.previous_applied_json = reserved[i];
                break;
            }
        if (operation.previous_applied_json.empty())
            operation.previous_applied_json = encode_camera_config(previous, old_rows);
    }
    if (!running_)
        return reject(1003, "configuration service is stopping");
    if (!db_.submit_camera_config(target, rows, operation, state.desired_revision))
        return reject(5001, "persist desired configuration failed");
    result.accepted = true;
    result.camera_id = operation.camera_id;
    result.revision = operation.target_revision;
    result.operation_id = operation.operation_id;
    lease.reset(); // Never wait while holding the executor's lease.
    admission.unlock();
    reconciler_.wake();
    return wait(std::move(result));
}

ConfigurationResult CameraService::wait(ConfigurationResult result) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, params_.apply_wait_ms));
    for (;;) {
        ConfigOperationRow operation;
        if (!db_.get_config_operation(result.operation_id, operation)) {
            result.code = 1003;
            result.apply_status = "unknown";
            result.message = "configuration saved; result temporarily unavailable";
            return result;
        }
        result.apply_status = operation.status;
        if (operation.status == "succeeded") {
            result.code = 0;
            result.message = "ok";
            return result;
        }
        if (operation.status == "failed") {
            result.code = operation.error_code ? operation.error_code : 1003;
            result.message = operation.last_error;
            return result;
        }
        if (operation.status == "superseded") {
            result.code = 1003;
            result.message = "configuration replaced by a newer target";
            return result;
        }
        if (!running_ || std::chrono::steady_clock::now() >= deadline) {
            result.code = 1003;
            result.message = "configuration saved; awaiting application";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace biz
} // namespace aibox
