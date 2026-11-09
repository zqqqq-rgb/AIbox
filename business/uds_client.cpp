#include "uds_client.h"
#include "task_compiler.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <json/json.h>

#include "common/Log.h"
#include "config_compiler.h"

namespace aibox {
namespace biz {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* state_name(UdsClient::State s) {
    switch (s) {
        case UdsClient::State::DISCONNECTED: return "DISCONNECTED";
        case UdsClient::State::SYNCING: return "SYNCING";
        case UdsClient::State::ONLINE: return "ONLINE";
    }
    return "?";
}

}  // namespace

UdsClient::UdsClient(std::string sock_path, Db& db, Hooks hooks)
    : UdsClient(std::move(sock_path), db, std::move(hooks), Params{}) {}

UdsClient::UdsClient(std::string sock_path, Db& db, Hooks hooks, Params p)
    : sock_path_(std::move(sock_path)), db_(db), hooks_(std::move(hooks)), p_(p) {}

UdsClient::~UdsClient() { stop(); }

void UdsClient::start() {
    if (running_.exchange(true)) return;  // 幂等
    th_ = std::thread([this] { loop(); });
}

void UdsClient::stop() {
    if (!running_.exchange(false)) return;  // 幂等
    disconnect("stopped");
    if (th_.joinable()) th_.join();
}

std::unique_ptr<UdsClient::ConfigurationLease> UdsClient::begin_configuration() {
    std::unique_lock<std::mutex> lock(configuration_mu_, std::try_to_lock);
    if (!lock.owns_lock() || !online()) return nullptr;
    return std::unique_ptr<ConfigurationLease>(new ConfigurationLease(*this, std::move(lock)));
}

void UdsClient::disconnect(const char* reason) {
    set_state(State::DISCONNECTED);
    ++epoch_;
    fail_all_pending(reason);  // Wake the lease holder BEFORE waiting on any transport lock.
    close_sock();
}

UdsClient::SyncStats UdsClient::sync_stats() const {
    std::lock_guard<std::mutex> lk(mu_stats_);
    return stats_;
}

void UdsClient::set_state(State s) {
    const State old = state_.exchange(s, std::memory_order_relaxed);
    if (old != s)
        LOGI("uds-client", "state %s -> %s (%s)", state_name(old), state_name(s),
             sock_path_.c_str());
}

// ---- socket 基础 ----

bool UdsClient::connect_once() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock_path_.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        LOGE("uds-client", "uds path too long: %s", sock_path_.c_str());
        return false;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path_.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;  // engine 未起/未 listen：上层按 connect_retry_ms 重试
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);  // pump 用 poll+MSG_DONTWAIT
    // 持 send_mu_ 落 fd：防 stop() 的 close_sock exchange(-1) 与 store 竞态漏关 fd
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        if (!running_.load()) { ::close(fd); return false; }
        sock_.store(fd, std::memory_order_relaxed);
    }
    return true;
}

void UdsClient::close_sock() {
    std::lock_guard<std::mutex> lk(send_mu_);
    const int fd = sock_.exchange(-1, std::memory_order_relaxed);
    if (fd >= 0) ::close(fd);
}

bool UdsClient::send_line(const std::string& line, uint64_t expected_epoch) {
    std::lock_guard<std::mutex> lk(send_mu_);
    const int fd = sock_.load(std::memory_order_relaxed);
    if (fd < 0 || (expected_epoch && (expected_epoch != connection_epoch() || !online()))) return false;
    std::string buf = line;
    buf.push_back('\n');  // JSON-lines 帧定界
    size_t off = 0;
    const int64_t deadline = now_ms() + p_.ack_timeout_ms;
    while (off < buf.size()) {
        if (!running_.load() || now_ms() >= deadline) {
            ::shutdown(fd, SHUT_RDWR);  // A truncated JSON line cannot be reused.
            return false;
        }
        const ssize_t n = ::send(fd, buf.data() + off, buf.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd writable{fd, POLLOUT, 0};
            ::poll(&writable, 1, 10);
            continue;
        }
        return false;  // EPIPE/ECONNRESET/EBADF：断连，上层转 DISCONNECTED
    }
    return true;
}

bool UdsClient::pump(int timeout_ms) {
    const int fd = sock_.load(std::memory_order_relaxed);
    if (fd < 0) return false;
    pollfd pf{};
    pf.fd = fd;
    pf.events = POLLIN;
    const int pr = ::poll(&pf, 1, timeout_ms);
    if (pr < 0) return errno == EINTR;  // 信号打断：不算断连
    if (pr == 0) return true;           // 超时：连接视为可用
    if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;

    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            rbuf_.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;  // 对端关闭
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }
    size_t pos = 0;
    for (;;) {
        const size_t nl = rbuf_.find('\n', pos);
        if (nl == std::string::npos) break;
        handle_line(rbuf_.substr(pos, nl - pos));
        pos = nl + 1;
    }
    rbuf_.erase(0, pos);
    return true;
}

void UdsClient::handle_line(const std::string& line) {
    proto::ParsedMsg pm;
    if (proto::decode_line(line, pm) != proto::kOk) {
        LOGW("uds-client", "engine 发来坏行（丢弃）: %.80s", line.c_str());
        return;
    }
    if (pm.kind == "ack") {
        std::lock_guard<std::mutex> lk(ack_mu_);
        auto it = pending_.find(pm.ack.ack);
        if (it != pending_.end() && !it->second.done) {
            it->second.done = true;
            it->second.ack = std::move(pm.ack);
            ack_cv_.notify_all();
        }
        return;
    }
    if (pm.kind == "evt") {
        // SYNCING/DISCONNECTED 期丢弃时序敏感增量事件（engine 重启场景旧事件
        // 无意义，详设 §5.2）；registry_ready 例外——它是幂等全量快照（整表
        // replace 无陈旧性），且 engine 的 on_connect 补发恰好落在 SYNCING
        // 窗口（连接建立即补发，冒烟实证：丢弃则 algorithms 表恒空）
        const bool is_snapshot = (pm.name == "registry_ready");
        if ((state() == State::ONLINE || is_snapshot) && hooks_.on_evt) {
            hooks_.on_evt(pm, line);
        }
    }
}

void UdsClient::fail_all_pending(const char* reason) {
    std::lock_guard<std::mutex> lk(ack_mu_);
    for (auto& kv : pending_) {
        if (!kv.second.done) {
            kv.second.done = true;
            kv.second.ack.ret = -1;
            kv.second.ack.msg = reason;
        }
    }
    ack_cv_.notify_all();
}

// ---- 等 ack（两条路径）----

void UdsClient::arm_pending(uint64_t msg_id) {
    std::lock_guard<std::mutex> lk(ack_mu_);
    pending_.emplace(msg_id, Pending{});
}

void UdsClient::disarm_pending(uint64_t msg_id) {
    std::lock_guard<std::mutex> lk(ack_mu_);
    pending_.erase(msg_id);
}

// client 线程自身使用：不能 cv 阻塞（自己就是分发者）——pump 轮询驱动分发
bool UdsClient::wait_ack_here(uint64_t msg_id, proto::Ack& out, int timeout_ms) {
    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(ack_mu_);
            auto it = pending_.find(msg_id);
            if (it != pending_.end() && it->second.done) {
                out = std::move(it->second.ack);
                pending_.erase(it);
                return true;
            }
        }
        if (!running_.load()) {
            std::lock_guard<std::mutex> lk(ack_mu_);
            pending_.erase(msg_id);
            out.ret = -1;
            out.msg = "stopped";
            return false;
        }
        if (now_ms() >= deadline) {
            std::lock_guard<std::mutex> lk(ack_mu_);
            pending_.erase(msg_id);
            return false;
        }
        if (!pump(50)) { disarm_pending(msg_id); return false; }
    }
}

// 跨线程等待（HttpApi 线程）：client 线程 pump 分发 ack 后 notify 唤醒
proto::Ack UdsClient::request_for(uint64_t msg_id, const std::string& line, int timeout_ms) {
    const auto expected_epoch = connection_epoch();
    {
        std::lock_guard<std::mutex> lk(ack_mu_);
        pending_[msg_id] = {};
    }
    proto::Ack out;
    out.ret = -1;
    out.msg = "not connected";
    if (!send_line(line, expected_epoch)) {
        std::lock_guard<std::mutex> lk(ack_mu_);
        pending_.erase(msg_id);
        return out;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lk(ack_mu_);
    ack_cv_.wait_until(lk, deadline, [&] {
        auto it = pending_.find(msg_id);
        return it == pending_.end() || it->second.done;
    });
    auto it = pending_.find(msg_id);
    if (it != pending_.end()) {
        if (it->second.done) out = std::move(it->second.ack);
        pending_.erase(it);
    }
    return out;
}

// ---- 运行期命令（cmd_mu_ 全局串行 = 因果序）----

namespace {

proto::Ack offline_ack() {
    proto::Ack a;
    a.ret = -1;
    a.msg = "engine not online";
    return a;
}

}  // namespace

proto::Ack UdsClient::add_cam(const proto::AddCam& v) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!online()) return offline_ack();
    const uint64_t mid = next_msg_id();
    return request_for(mid, proto::encode_request(mid, v), p_.ack_timeout_ms);
}

proto::Ack UdsClient::set_algos(const proto::SetAlgos& v) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!online()) return offline_ack();
    if (v.config_version == 2) {
        std::string error;
        if (!proto::validate_v2_config(v,error)) { proto::Ack a; a.ret=1001; a.msg=error; return a; }
        const auto capability_id = next_msg_id();
        const auto caps = request_for(capability_id, proto::encode_request(capability_id,proto::GetCapabilities{}),p_.ack_timeout_ms);
        if (caps.ret!=0 || !caps.has_data || !proto::supports_rule_v2(caps.data)) {
            proto::Ack a; a.ret=1003; a.msg="engine does not advertise V2 rule support"; return a;
        }
    }
    const uint64_t mid = next_msg_id();
    return request_for(mid, proto::encode_request(mid, v), p_.ack_timeout_ms);
}

proto::Ack UdsClient::remove_cam(const proto::RemoveCam& v) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!online()) return offline_ack();
    const uint64_t mid = next_msg_id();
    return request_for(mid, proto::encode_request(mid, v), p_.ack_timeout_ms);
}

proto::Ack UdsClient::reload_algo(const proto::ReloadAlgo& v) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!online()) return offline_ack();
    const uint64_t mid = next_msg_id();
    return request_for(mid, proto::encode_request(mid, v), p_.ack_timeout_ms);
}

bool UdsClient::get_metrics(std::string* data_out, int timeout_ms) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!online()) return false;
    const uint64_t mid = next_msg_id();
    // REST 请求不宜等满 5s：显式传调用方超时（不共享可变 Params，无线程竞争）
    proto::Ack a = request_for(mid, proto::encode_request(mid, proto::GetMetrics{}), timeout_ms);
    if (a.ret != 0) return false;
    if (data_out) *data_out = std::move(a.data);
    return true;
}

bool UdsClient::get_capabilities(std::string* data_out, int timeout_ms) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!online()) return false;
    const auto mid = next_msg_id();
    auto ack = request_for(mid, proto::encode_request(mid,proto::GetCapabilities{}),timeout_ms);
    if (ack.ret!=0 || !ack.has_data) return false;
    if (data_out) *data_out=std::move(ack.data);
    return true;
}

bool UdsClient::sub_meta(uint32_t cam, bool subscribe) {
    {
        std::lock_guard<std::mutex> lk(ack_mu_);
        if (subscribe)
            active_subs_.insert(cam);
        else
            active_subs_.erase(cam);
    }
    proto::SubMeta m;
    m.id = cam;
    m.subscribe = subscribe;
    return send_line(proto::encode_request(next_msg_id(), m));  // fire-and-forget
}

// ---- SYNCING（client 线程内执行）----

bool UdsClient::do_sync() {
    SyncStats st;
    // Recompute before replay: the previous window may have expired offline.
    if (!db_.refresh_task_schedules(p_.schedule_now ? p_.schedule_now() : task_now_ms())) return false;
    if (!db_.invalidate_runtime_config_states()) return false;
    // 1. ZLM 代理重建（详设 §5.7-5：getMediaList 对比 DB，ZLM 重启过则逐台重建）
    if (hooks_.ensure_proxies) {
        ++st.ensure_proxies_calls;
        if (!hooks_.ensure_proxies())
            LOGW("uds-client", "ensure_proxies 失败（继续同步；engine 拉流失败会自愈重试）");
    }
    // 2. 读 DB
    const auto cams = db_.list_cameras(false, true);
    int degraded = 0;
    bool capabilities_checked=false, supports_v2=false;
    // 3. 逐台 add_cam → set_algos（失败重试 ack_retries 次；degraded 继续下一台）
    for (const auto& cam : cams) {
        bool cam_ok = true;
        CameraConfigState config_state;
        if (!db_.get_camera_config_state(cam.id, config_state)) return false;
        const auto rows = db_.list_camera_algos(cam.id);
        CameraConfigSnapshot snapshot{cam, rows, config_state.deleted_at != 0};
        CompiledCameraConfig compiled;
        std::string compile_error;
        ConfigOperationRow desired;
        const bool loaded = db_.get_current_config_operation(cam.id,desired)
            ? decode_camera_config(desired.target_config_json,snapshot)
            : build_camera_config(db_,cam,rows,config_state.deleted_at!=0,snapshot,compile_error);
        if (!loaded || snapshot.camera.id!=cam.id || !compile_camera_config(snapshot, p_.media_host, p_.media_rtsp_port,
                                   compiled, compile_error)) {
            LOGW("uds-client", "cam %lld invalid configuration: %s",
                 static_cast<long long>(cam.id), compile_error.c_str());
            ++st.set_algos_fail;
            ++degraded;
            if (!db_.request_camera_replay(snapshot)) return false;
            continue;  // Do not silently replace a broken ROI with full-frame detection.
        }
        if (compiled.algos.config_version==2) {
            if(!capabilities_checked) {
                capabilities_checked=true;
                const auto mid=next_msg_id(); arm_pending(mid);
                if(!send_line(proto::encode_request(mid,proto::GetCapabilities{}))) { disarm_pending(mid); return false; }
                proto::Ack ack;
                supports_v2=wait_ack_here(mid,ack,p_.ack_timeout_ms) && ack.ret==0 && ack.has_data && proto::supports_rule_v2(ack.data);
            }
            if(!supports_v2) {
                ++degraded; ++st.set_algos_fail;
                if(!db_.request_camera_replay(snapshot)) return false;
                continue;
            }
        }
        if (hooks_.prepare_camera && !hooks_.prepare_camera(snapshot)) {
            ++degraded;
            if (!db_.request_camera_replay(snapshot)) return false;
            continue;
        }
        if (!cam.enabled || snapshot.deleted) {
            proto::RemoveCam remove;
            remove.id = static_cast<uint32_t>(cam.id);
            bool stopped = false;
            for (int attempt = 0; attempt <= p_.ack_retries && !stopped; ++attempt) {
                const auto mid = next_msg_id();
                arm_pending(mid);
                if (!send_line(proto::encode_request(mid, remove))) { disarm_pending(mid); return false; }
                proto::Ack ack;
                stopped = wait_ack_here(mid, ack, p_.ack_timeout_ms) && (ack.ret == 0 || ack.ret == 1002);
            }
            if (stopped) {
                db_.set_camera_online(cam.id, false);
                if (!db_.confirm_camera_replay(cam.id, config_state.desired_revision,
                        encode_camera_config(snapshot))) {
                    ++degraded;
                    if (!db_.request_camera_replay(snapshot)) return false;
                }
            } else {
                ++degraded;
                if (!db_.request_camera_replay(snapshot)) return false;
            }
            continue;
        }
        const auto& ac = compiled.add;
        const auto& sa = compiled.algos;
        bool added = false;
        bool rebuilt = false;
        proto::Ack ack;
        for (int attempt = 0; attempt <= p_.ack_retries && !added; ++attempt) {
            const uint64_t mid = next_msg_id();
            arm_pending(mid);  // 先注册后发送：快 ack 到达时 handle_line 才有归属
            if (!send_line(proto::encode_request(mid, ac))) {
                disarm_pending(mid);
                return false;  // 断线→重连重同步
            }
            if (wait_ack_here(mid, ack, p_.ack_timeout_ms)) {
                if (ack.ret == 0) added = true;
                else if (ack.ret == 1003 && !rebuilt) {
                    // The engine may have survived a businessd restart. A duplicate ACK
                    // cannot prove its URL/algorithms match this database snapshot.
                    rebuilt = true;
                    proto::RemoveCam remove;
                    remove.id = ac.id;
                    const uint64_t remove_mid = next_msg_id();
                    arm_pending(remove_mid);
                    if (!send_line(proto::encode_request(remove_mid, remove))) {
                        disarm_pending(remove_mid);
                        return false;
                    }
                    proto::Ack removed;
                    if (!wait_ack_here(remove_mid, removed, p_.ack_timeout_ms) ||
                        (removed.ret != 0 && removed.ret != 1002)) break;
                    --attempt;  // One bounded rebuild, including ack_retries=0.
                }
            }
        }
        if (added) ++st.add_cam_ok; else ++st.add_cam_fail;
        if (added) {
            // online 直接置位：SYNCING 期 engine 的 camera_status 应答事件按
            // 详设 §5.2 丢弃（防旧事件），但本台 add 刚 ack 成功是权威事实——
            // 若依赖事件，ONLINE 后再无 camera_status，online 恒 0（冒烟实证）
            db_.set_camera_online(cam.id, true);
            bool applied = false;
            for (int attempt = 0; attempt <= p_.ack_retries && !applied; ++attempt) {
                const uint64_t mid = next_msg_id();
                arm_pending(mid);
                if (!send_line(proto::encode_request(mid, sa))) {
                    disarm_pending(mid);
                    return false;
                }
                if (wait_ack_here(mid, ack, p_.ack_timeout_ms) && ack.ret == 0) applied = true;
            }
            if (applied) ++st.set_algos_ok; else ++st.set_algos_fail;
            if (!applied || !db_.confirm_camera_replay(cam.id, config_state.desired_revision,
                                        encode_camera_config(snapshot))) cam_ok = false;
        } else {
            cam_ok = false;
        }
        if (!cam_ok) {
            ++degraded;
            if (!db_.request_camera_replay(snapshot)) return false;
        }
    }
    st.degraded_cams = degraded;
    {
        std::lock_guard<std::mutex> lk(mu_stats_);
        stats_ = st;
    }
    LOGI("uds-client", "sync done: cams=%zu add ok/fail=%d/%d algos ok/fail=%d/%d degraded=%d",
         cams.size(), st.add_cam_ok, st.add_cam_fail, st.set_algos_ok, st.set_algos_fail,
         degraded);
    return true;  // 断线才返回 false；degraded 通道不算同步失败
}

// ---- 主循环 ----

void UdsClient::loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> replay_lock(configuration_mu_, std::defer_lock);
        while (running_.load() && !replay_lock.try_lock())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!running_.load()) break;
        // DISCONNECTED → connect（1s 重试，详设 §5.2）
        if (!connect_once()) {
            set_state(State::DISCONNECTED);
            const int64_t until = now_ms() + p_.connect_retry_ms;
            while (running_.load() && now_ms() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        rbuf_.clear();
        set_state(State::SYNCING);
        if (!do_sync() || !running_.load()) {
            disconnect("disconnected");
            continue;
        }
        set_state(State::ONLINE);
        replay_lock.unlock();

        // 重连后重发订阅（engine 重启不丢 WsHub 订阅；详设 §5.3 UDS 全量重同步语义）
        std::vector<uint32_t> subs;
        {
            std::lock_guard<std::mutex> lk(ack_mu_);
            subs.assign(active_subs_.begin(), active_subs_.end());
        }
        for (uint32_t cam : subs) {
            proto::SubMeta m;
            m.id = cam;
            m.subscribe = true;
            send_line(proto::encode_request(next_msg_id(), m));
        }

        // ONLINE：收包 + 心跳（get_metrics 口径；连续超时 → 断连重同步）
        int64_t last_ping = now_ms();
        int ping_fails = 0;
        uint64_t ping_mid = 0;
        int64_t ping_sent_ms = 0;
        bool alive = true;
        while (alive && running_.load()) {
            if (!pump(100)) break;
            const int64_t now = now_ms();
            // 心跳 ack 到达 → 喂活
            if (ping_mid != 0) {
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(ack_mu_);
                    auto it = pending_.find(ping_mid);
                    if (it != pending_.end() && it->second.done) {
                        pending_.erase(it);
                        done = true;
                    }
                }
                if (done) {
                    ping_fails = 0;
                    ping_mid = 0;
                } else if (now - ping_sent_ms >= p_.ping_interval_ms) {
                    // 一个完整周期无 ack → 记一次失败
                    std::lock_guard<std::mutex> lk(ack_mu_);
                    pending_.erase(ping_mid);
                    ping_mid = 0;
                    if (++ping_fails >= p_.ping_fail_limit) {
                        LOGW("uds-client", "心跳连续 %d 次超时 → 断线重同步", ping_fails);
                        alive = false;
                    }
                }
            }
            if (alive && ping_mid == 0 && now - last_ping >= p_.ping_interval_ms) {
                last_ping = now;
                ping_mid = next_msg_id();
                ping_sent_ms = now;
                arm_pending(ping_mid);
                if (!send_line(proto::encode_request(ping_mid, proto::GetMetrics{}))) {
                    alive = false;
                }
            }
        }
        disconnect("disconnected");
    }
}

}  // namespace biz
}  // namespace aibox
