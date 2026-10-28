#include "janitor.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <json/json.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/Log.h"

namespace aibox {
namespace biz {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 默认使用率：statvfs（已用/总量，f_bavail 视作可用）；
// 注意 statvfs 在 POSIX 是函数名，结构体须写 struct statvfs
double statvfs_usage(const std::string& dir) {
    struct statvfs st{};
    if (::statvfs(dir.c_str(), &st) != 0) return -1.0;  // 异常：调用方跳过
    if (st.f_blocks == 0) return -1.0;
    const double used = static_cast<double>(st.f_blocks - st.f_bavail);
    return used / static_cast<double>(st.f_blocks);
}

// 真实删除文件：ENOENT 视为已删（幂等）
bool unlink_file(const std::string& path) {
    if (::unlink(path.c_str()) == 0) return true;
    return errno == ENOENT;
}

}  // namespace

// ---- DiskJanitor ----

DiskJanitor::DiskJanitor(Db& db, Params p, UsageFn usage_fn)
    : db_(db), p_(p), usage_fn_(std::move(usage_fn)) {}

DiskJanitor::~DiskJanitor() { stop(); }

void DiskJanitor::start() {
    if (running_.exchange(true)) return;
    th_ = std::thread([this] {
        while (running_.load()) {
            sweep_once();
            const int64_t until = now_ms() + p_.check_interval_s * 1000;
            while (running_.load() && now_ms() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void DiskJanitor::stop() {
    if (!running_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

int DiskJanitor::sweep_once() {
    int removed = sweep_expired();
    double usage = -1.0;
    if (usage_fn_) {
        usage = usage_fn_(p_.mp4_dir);
    } else if (!p_.mp4_dir.empty()) {
        usage = statvfs_usage(p_.mp4_dir);
    }
    if (usage >= 0) removed += sweep_watermark(usage);
    removed += sweep_snap_expired();
    if (removed > 0) LOGI("janitor", "sweep 删除 %d 个切片文件", removed);
    return removed;
}

// 保留天数清理：start_ts 早于 now - keep_days
int DiskJanitor::sweep_expired() {
    if (p_.keep_days <= 0) return 0;
    const int64_t cutoff = now_ms() - static_cast<int64_t>(p_.keep_days) * 86400000;
    int removed = 0;
    while (removed < p_.batch_max) {
        const auto olds = db_.oldest_segments(1);
        if (olds.empty() || olds[0].start_ts >= cutoff) break;
        if (!unlink_file(olds[0].file_path))
            LOGW("janitor", "unlink 失败: %s", olds[0].file_path.c_str());
        db_.delete_segment(olds[0].id);
        ++removed;
    }
    return removed;
}

// 水位兜底：usage > high → 删最旧直至 ≤high（受 batch_max 约束）
int DiskJanitor::sweep_watermark(double usage) {
    int removed = 0;
    while (usage > p_.high_watermark && removed < p_.batch_max) {
        const auto olds = db_.oldest_segments(1);
        if (olds.empty()) {
            LOGW("janitor", "水位 %.0f%% 超限但 segments 已空（录像目录被外力填充？）",
                 usage * 100.0);
            break;
        }
        if (!unlink_file(olds[0].file_path))
            LOGW("janitor", "unlink 失败: %s", olds[0].file_path.c_str());
        db_.delete_segment(olds[0].id);
        ++removed;
        // 重算使用率（注入函数按删除进度更新；真实 statvfs 反映 unlink 后状态）
        usage = usage_fn_ ? usage_fn_(p_.mp4_dir) : statvfs_usage(p_.mp4_dir);
        if (usage < 0) break;
    }
    return removed;
}

// 事件快照保留天数清理：mtime 早于 cutoff 的 .jpg（目录遍历，无 DB 行——
// 快照文件名自带 cam/ts，DB 回填的 snapshot_path 失效后前端 onerror 兜底显示）
int DiskJanitor::sweep_snap_expired() {
    if (p_.keep_days <= 0 || p_.snap_dir.empty()) return 0;
    DIR* d = ::opendir(p_.snap_dir.c_str());
    if (!d) return 0;  // 目录尚未创建（尚无事件快照）
    const int64_t cutoff_ms = now_ms() - static_cast<int64_t>(p_.keep_days) * 86400000;
    int removed = 0;
    while (removed < p_.batch_max) {
        dirent* ent = ::readdir(d);
        if (!ent) break;
        const std::string name = ent->d_name;
        if (name.size() < 4 || name.rfind(".jpg") != name.size() - 4) continue;
        const std::string full = p_.snap_dir + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const int64_t mtime_ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 +
                                 st.st_mtim.tv_nsec / 1000000;
        if (mtime_ms >= cutoff_ms) continue;
        if (!unlink_file(full)) LOGW("janitor", "unlink 快照失败: %s", full.c_str());
        ++removed;
    }
    ::closedir(d);
    return removed;
}

// ---- OutboxRetransmitter ----

OutboxRetransmitter::OutboxRetransmitter(Db& db, IMqttPublisher* pub)
    : OutboxRetransmitter(db, pub, Params{}) {}

OutboxRetransmitter::OutboxRetransmitter(Db& db, IMqttPublisher* pub, Params p)
    : db_(db), pub_(pub), p_(p) {}

OutboxRetransmitter::~OutboxRetransmitter() { stop(); }

void OutboxRetransmitter::start() {
    if (running_.exchange(true)) return;
    th_ = std::thread([this] {
        while (running_.load()) {
            drain_once();
            const int64_t until = now_ms() + p_.interval_ms;
            while (running_.load() && now_ms() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void OutboxRetransmitter::stop() {
    if (!running_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

int OutboxRetransmitter::drain_once() {
    if (!pub_) return 0;
    // 注意：不检查 running_——drain_once 是可独立调用的排水工具（单测直调、
    // 上层线程循环保留 running_ 判定），与线程生命周期解耦（板上实测踩中：
    // 未 start 直调时 running_=false，首轮即 break，恒 0 条）
    const auto rows = db_.pending_outbox(p_.batch);
    int acked = 0;
    for (const auto& r : rows) {
        if (!pub_->publish(r.topic, r.payload)) {
            LOGW("janitor", "mqtt 未确认（保留重传）: topic=%s", r.topic.c_str());
            break;  // 保序：前面的未确认，后面这轮不再发
        }
        // 对账：payload._event_id → events.reported=1
        Json::Value payload;
        Json::CharReaderBuilder b;
        std::unique_ptr<Json::CharReader> rd(b.newCharReader());
        std::string errs;
        if (rd->parse(r.payload.data(), r.payload.data() + r.payload.size(), &payload,
                      &errs) &&
            payload.isMember("_event_id")) {
            db_.mark_event_reported(payload["_event_id"].asInt64());
        }
        db_.delete_outbox(r.id);
        ++acked;
    }
    if (acked > 0) LOGI("janitor", "outbox 重传确认 %d 条", acked);
    return acked;
}

}  // namespace biz
}  // namespace aibox
