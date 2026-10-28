// RknnEngine —— IInferEngine 板端实现（任务分解 T3.4，详设 §3.5/3.6，demo rknn_infer 产品化）
//
// demo 确定性坑内建（任务分解 §7 附表 K10~K12）：
//   K10 rknn_query attr / rknn_outputs_get 前必须设 .index=i（否则全查第 0 个
//       张量 → 反量化对 sum 分支越界读 → SIGSEGV）
//   K11 rknn_init 后显式 rknn_set_core_mask 绑核（AUTO=驱动随机单核，等于
//       只用 1/3 算力且破坏 Core0/1 分区纪律，架构 §7.3）
//   K12 npu_cost_ms 口径 = 仅 rknn_run 段（last_run_us），不含 outputs_get/反量化
//
// 输入：UINT8 NHWC RGB888（zoo 模型实测格式，0~255 归一化由模型图承载）；
// 输出：want_float=0 取原始量化张量，按 INT8 affine / FP16 反量化为 float
//       （zoo 部分分支保留 FP16，硬当 INT8 会算出错误坐标）
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "interfaces/iinfer.h"

namespace aibox {

class RknnEngine : public IInferEngine {
public:
    // manifest.core → NPU 物理核（steady=Core0 常驻 / triggered=Core1）
    enum class Core { Steady, Triggered };

    RknnEngine();
    ~RknnEngine() override;  // Impl 前置声明，析构需完整类型

    // 加载 zoo 9 输出 YOLOv8：rknn_init → K10 逐个查询 attr → 校验输出数
    // 非 9 输出视为非 zoo 版模型（模型转换手册 §4.1），load 失败
    bool load(const std::string& model_path) override;

    // K11 绑核：load 之后、首帧之前调用（角色语义 Steady=常驻 / Triggered=触发）
    bool set_core(Core c);

    // NpuCorePlan 分核：显式物理核 id（0/1/2 → RKNN_NPU_CORE_0/1/2）。
    // 仅单核掩码——0_1/0_1_2 等多核协同会让单次推理独占全 NPU、饿死常驻检测（架构 §7.3）
    bool set_core_index(int core_id);

    bool run(const InferTensor& in, std::vector<std::vector<float>>& outputs) override;

    // YoloPostConfig 组装用：9 输出的 NCHW [h,w]（load 后有效；下标=输出序）
    const std::vector<std::pair<uint32_t, uint32_t>>& out_hw() const { return out_hw_; }

    // 模型输入尺寸（attr 实测，与 manifest 一致性校验用；load 后有效）
    uint32_t in_w() const { return in_w_; }
    uint32_t in_h() const { return in_h_; }

    uint32_t mem_kb() const { return mem_kb_; }  // 老 runtime 无 MEM_SIZE 查询，恒 0

    // K12 口径：最近一次 run 的 rknn_run 段耗时（us），npu_cost_ms 回填用
    uint64_t last_run_us() const { return last_run_us_.load(std::memory_order_relaxed); }

    // load/run/set_core 失败的人类可读原因（ModelRegistry.disabled_reason 透传）
    const std::string& last_error() const { return err_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::vector<std::pair<uint32_t, uint32_t>> out_hw_;
    uint32_t in_w_ = 0, in_h_ = 0;
    uint32_t mem_kb_ = 0;
    std::atomic<uint64_t> last_run_us_{0};
    std::string err_;
};

}  // namespace aibox
