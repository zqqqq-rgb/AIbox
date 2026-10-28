#include "model/ModelRegistry.h"

#include "common/Log.h"

#include <chrono>
#include <filesystem>
#include <utility>

#include "infer/Yolov8Post.h"

namespace aibox {

namespace fs = std::filesystem;

namespace {
inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::string dir_name(const std::string& dir) {
    return fs::path(dir).filename().string();
}
}  // namespace

ModelRegistry::ModelRegistry(std::string packages_root, RegistryDeps deps)
    : root_(std::move(packages_root)), deps_(std::move(deps)) {}

bool ModelRegistry::selftest(const Entry& e, std::string& err) {
    if (!e.inst || !e.inst->infer || !e.inst->preproc) {
        err = "selftest: instance incomplete";
        return false;
    }
    // 零张量推理冒烟：init/attr 之外的运行期错误（内存不足/核绑定失败）在此时暴露
    InferTensor t;
    t.width = e.inst->in_w;
    t.height = e.inst->in_h;
    t.data.assign(static_cast<size_t>(t.width) * t.height * 3, 0);

    std::vector<std::vector<float>> outs;
    uint64_t t0 = now_ms();
    if (!e.inst->infer->run(t, outs)) {
        err = "selftest: infer.run fail";
        return false;
    }
    // post 冒烟：identity letterbox（scale=1, pad=0）下不崩溃且可调用
    LetterBoxParam lb;  // 全默认 = identity
    auto targets = e.inst->post ? e.inst->post(outs, lb)
                                : AlgoInstance::default_post(outs, lb);
    (void)targets;
    (void)(now_ms() - t0);  // selftest 耗时仅日志参考；权威 npu_cost_ms 来自 npu_bench
    return true;
}

bool ModelRegistry::build(const std::string& dir, Entry& e, std::string& err) {
    e = Entry{};
    e.pkg = validate_package(dir);
    if (!e.pkg.ok) {
        err = e.pkg.error;
        return false;
    }
    const auto& m = e.pkg.manifest;

    // ctx 内存估算：模型文件体积（KB，向上取整）。板上 librknnrt 无
    // RKNN_QUERY_MEM_SIZE 实测通道（RknnEngine 板上实证），权重占 ctx 内存
    // 大头，文件体积是可用近似——自动计算，无需 manifest 声明或打包回填。
    // 失败/坏包 = 0（前端显示 --，内存预检 factor2 按 0 计，不阻塞配置）
    {
        std::error_code ec;
        const auto bytes = fs::file_size(fs::path(dir) / m.model_file, ec);
        e.mem_kb = ec ? 0 : static_cast<uint32_t>((bytes + 1023) / 1024);
    }

    auto infer = deps_.make_infer ? deps_.make_infer(m, dir, &err) : nullptr;
    if (!infer) {
        if (err.empty()) err = "make_infer returned null";
        return false;
    }

    auto inst = std::make_shared<AlgoInstance>();
    inst->algo_id = m.algo_id;
    inst->in_w = m.in_w;
    inst->in_h = m.in_h;
    inst->infer = std::move(infer);
    inst->preproc = deps_.make_preproc ? deps_.make_preproc() : nullptr;
    if (!inst->preproc) {
        err = "make_preproc returned null";
        return false;
    }
    inst->post = deps_.make_post ? deps_.make_post(m, *inst->infer) : nullptr;

    e.inst = std::move(inst);
    if (!selftest(e, err)) {
        e.inst.reset();
        return false;
    }
    return true;
}

void ModelRegistry::load_all() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
    for (auto& pkg : scan_packages(root_)) {
        Entry e;
        std::string err;
        const std::string id =
            pkg.ok ? pkg.manifest.algo_id : dir_name(pkg.dir);
        if (pkg.ok && build(pkg.dir, e, err)) {
            // 消息先格式化再 move：emplace(std::move(e)) 后 e.pkg 已被搬空，
            // 先打印后移动会输出空 version（板上实测 "v loaded" 空版本的根因）
            const std::string loaded_msg =
                "[registry] " + id + " v" + e.pkg.manifest.version + " loaded (" + pkg.dir + ")";
            entries_.emplace(id, std::move(e));
            LOGI("registry", "%s", loaded_msg.c_str());
        } else {
            // 坏包/构建失败：disabled 条目保留原因，registry_ready 上报（不拖垮启动）
            Entry bad;
            bad.pkg = pkg;
            if (bad.pkg.ok) bad.pkg.ok = false;
            bad.pkg.error = pkg.ok ? err : pkg.error;
            const std::string bad_msg =
                "[registry] " + id + " DISABLED: " + bad.pkg.error;
            entries_.emplace(id, std::move(bad));
            LOGE("registry", "%s", bad_msg.c_str());
        }
    }
}

bool ModelRegistry::reload(const std::string& algo_id) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string dir = root_ + "/" + algo_id;
    const std::string bak = root_ + "/" + algo_id + ".bak";

    Entry e;
    std::string err;
    if (build(dir, e, err)) {
        entries_[algo_id] = std::move(e);
        LOGI("registry", "reload %s OK", algo_id.c_str());
        return true;
    }

    // 新包构建失败 → .bak 回滚（架构 §11.4.4）：备份恢复为在线包后重试一次
    std::error_code ec;
    if (fs::exists(bak, ec)) {
        LOGE("registry", "reload %s fail(%s), rollback .bak",
             algo_id.c_str(), err.c_str());
        fs::remove_all(dir, ec);
        fs::rename(bak, dir, ec);
        Entry e2;
        std::string err2;
        if (!ec && build(dir, e2, err2)) {
            entries_[algo_id] = std::move(e2);
            LOGI("registry", "rollback %s OK", algo_id.c_str());
            return true;
        }
        err = "reload fail and rollback fail: " + (err2.empty() ? err : err2);
    }

    // 新旧均不可用：该 algo 置 disabled（调度侧 get 返回 nullptr → 预检 1002）
    Entry bad;
    bad.pkg.dir = dir;
    bad.pkg.ok = false;
    bad.pkg.manifest.algo_id = algo_id;
    bad.pkg.error = err.empty() ? "reload failed" : err;
    entries_[algo_id] = std::move(bad);
    return false;
}

std::shared_ptr<AlgoInstance> ModelRegistry::get(const std::string& algo_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(algo_id);
    return it == entries_.end() ? nullptr : it->second.inst;
}

proto::RegistryReadyEvt ModelRegistry::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    proto::RegistryReadyEvt evt;
    evt.algos.reserve(entries_.size());
    for (const auto& [id, e] : entries_) {
        proto::RegistryAlgo a;
        a.algo_id = id;
        a.version = e.pkg.manifest.version;
        a.model_ref = e.pkg.manifest.model_file;
        a.trigger = e.pkg.manifest.core;
        a.npu_cost_ms = e.pkg.manifest.npu_cost_ms;
        a.npu_mem_kb = e.mem_kb;  // 模型文件体积估算（build 时自动计算）
        a.disabled = !e.inst;
        a.classes = e.pkg.manifest.classes;  // manifest 白名单透传（空 = 全类）
        a.class_names = e.pkg.manifest.class_names;  // id→名称映射透传
        evt.algos.push_back(std::move(a));
    }
    return evt;
}

float ModelRegistry::cost_ms_of(const std::string& algo_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(algo_id);
    return it == entries_.end() ? 0.f : it->second.pkg.manifest.npu_cost_ms;
}

size_t ModelRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

}  // namespace aibox
