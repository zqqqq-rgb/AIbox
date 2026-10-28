// ModelRegistry —— 模型注册表（任务分解 T3.3，详设 §3.5，架构 §11.4.2/11.4.4）
//
// 一个物理模型 = 一个 AlgoInstance（preproc + infer + post 闭包）。
// 逻辑算法（set_algos 的 algo 项）按 algo_id 引用实例；多个逻辑算法共享同一实例。
//
// 接缝设计：RknnEngine/RgaPreproc 等硬件件经 RegistryDeps 工厂注入，
// 本类零硬件头文件（core 层门禁），x86 用 SoftLetterbox+FakeInfer 工厂可全路径单测；
// 板上 main 装配注入 RgaPreproc+RknnEngine 工厂（详设 §3.11）。
//
// 构建链：validate_package（AlgoPackage）→ infer 工厂 load → 输入尺寸与 manifest
// 交叉校验 → set_core（K11 绑核分区纪律）→ selftest（零张量推理 + post 闭包冒烟，
// 失败 = disabled + 原因，坏包不得拖垮启动）。
// 热升级 reload：新包构建失败且存在 <root>/<algo_id>.bak → 回滚重试（架构 §11.4.4）。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "infer/AlgoInstance.h"
#include "ipc/Protocol.h"
#include "model/AlgoPackage.h"

namespace aibox {

class ModelRegistry {
public:
    // 硬件件工厂（板上 main 注入 RgaPreproc/RknnEngine；x86 测试注入 Soft/Fake）
    struct RegistryDeps {
        std::function<std::shared_ptr<IPreproc>()> make_preproc;
        // pkg_dir：包目录（模型文件定位 = pkg_dir/manifest.model_file）
        // 返回 nullptr = load 失败（load 错误经 err 传出，进 disabled_reason）
        std::function<std::shared_ptr<IInferEngine>(const AlgoManifest&,
                                                    const std::string& pkg_dir,
                                                    std::string* err)>
            make_infer;
        // 输出解码闭包工厂：板上取 RknnEngine::out_hw + make_yolov8_post；
        // 返回空闭包 = 引擎直出 [n,6] 原图坐标（如 FakeInfer），注册表落 default_post
        std::function<PostFn(const AlgoManifest&, const IInferEngine&)> make_post;
    };

    ModelRegistry(std::string packages_root, RegistryDeps deps);

    // 全量扫描 + 构建 + 自检。永不抛异常：坏包 → disabled 条目（registry_ready 上报）
    void load_all();

    // 热升级：重扫该包并重建；失败且 .bak 存在 → 回滚重试。
    // 返回 false = 新旧均不可用（该 algo 已 disabled，ret=3003/3004 语义由调用方定）
    bool reload(const std::string& algo_id);

    // 调度器/装配用：未启用或不存在 → nullptr（set_algos 预检 1002 语义）
    std::shared_ptr<AlgoInstance> get(const std::string& algo_id) const;

    // registry_ready 事件载荷（启动/热升级/回滚后快照）
    proto::RegistryReadyEvt snapshot() const;

    // NPU 预检（架构 §11.4.3 闸门1）所需的实测口径：Σcost×Σfps（businessd 计算，
    // engine 提供单模型 npu_cost_ms —— manifest 回填值或 selftest 实测，取其大）
    float cost_ms_of(const std::string& algo_id) const;

    size_t size() const;  // 已注册条目数（含 disabled）

private:
    struct Entry {
        AlgoPackage pkg;                      // manifest + 校验状态（ok/error）
        std::shared_ptr<AlgoInstance> inst;   // ok 时非空
        uint32_t mem_kb = 0;                  // ctx 内存估算 = 模型文件体积 KB（build 计算）
        float selftest_cost_ms = 0.f;         // selftest 实测 rknn_run 中位
    };

    // 构建 + 自检一个包目录；失败时填 err（不抛异常）
    bool build(const std::string& dir, Entry& e, std::string& err);
    bool selftest(const Entry& e, std::string& err);

    std::string root_;
    RegistryDeps deps_;
    mutable std::mutex mu_;               // 冷路径锁（load/reload/snapshot）
    std::map<std::string, Entry> entries_;  // key = algo_id
};

}  // namespace aibox
