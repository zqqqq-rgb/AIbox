// AlgoPackage —— 算法包扫描与 manifest 校验（任务分解 T3.3，详设 §3.5，架构 §11.4.2）
// 算法包 = packages 目录下的一个子目录：
//   <root>/<algo_id>/manifest.json
//   <root>/<algo_id>/<model_file>        （rknn 模型）
//   <root>/<algo_id>.bak/                （热升级回滚副本，ModelRegistry 管理）
// manifest.json 字段（§11.4.2 定稿）：
//   algo_id, version, platform("rk3588"), model_file,
//   input_w, input_h, num_cls, default_threshold, core("steady"|"triggered"),
//   npu_cost_ms（npu_bench 实测回填）
// 注：ctx 内存不在 manifest 中声明——加载时按模型文件体积自动估算（ModelRegistry）
// 校验任一不过 → disabled + 原因（不抛异常，坏包不得拖垮扫描）
// 纯逻辑（FS + jsoncpp），x86 CI 全路径可测
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aibox {

// 当前目标平台（CMake -DAIBOX_TARGET_PLATFORM=rk3588 注入；与 manifest.platform 比对）
#ifndef AIBOX_TARGET_PLATFORM
#define AIBOX_TARGET_PLATFORM "rk3588"
#endif

struct AlgoManifest {
    std::string algo_id;
    std::string version;
    std::string platform;   // 必须等于 AIBOX_TARGET_PLATFORM
    std::string model_file; // 相对包目录
    uint32_t in_w = 640, in_h = 640;
    int num_cls = 80;
    float threshold = 0.45f;
    std::string core = "steady";  // steady（NPU Core0）| triggered（NPU Core1）
    float npu_cost_ms = 0.f;
    // 类别白名单（COCO id）：空 = 全类输出（兼容存量包）；非空 = 只输出这些类
    // （解码阶段过滤，Tracker/规则/前端/事件自动只见到白名单目标）
    std::vector<int> classes;
    bool has_classes = false;  // manifest 是否声明了 classes（区分"未声明=全类"与"[]"）
    // 类别名映射（可选，COCO id → 显示名）：与 classes 数组一一对应或覆盖全类。
    // 空 = 前端显示裸 id；提供时 registry_ready 透传 → 前端芯片/检测框标签显示
    std::vector<std::string> class_names;
};

struct AlgoPackage {
    std::string dir;        // 包目录绝对/相对路径
    bool ok = false;        // manifest 校验通过（模型存在性在 ok 内一并校验）
    AlgoManifest manifest;
    std::string error;      // !ok 时的人类可读原因（registry_ready.disabled_reason）
};

// 扫描 root 下所有子目录（跳过 *.bak），逐个校验；root 不存在返回空
std::vector<AlgoPackage> scan_packages(const std::string& root);

// 单包校验（reload 热升级路径复用同一规则）
AlgoPackage validate_package(const std::string& dir);

}  // namespace aibox
