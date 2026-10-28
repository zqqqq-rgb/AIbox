#include "hw/RknnEngine.h"

#include <rknn_api.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace aibox {

namespace {

// FP16(half) → float 位转换（demo pipeline.cpp 同款标准公式）
inline float fp16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
    uint32_t bits;
    if (exp == 0)
        bits = (sign << 31) | (frac << 13);  // subnormal → denormal 近似
    else if (exp == 31)
        bits = (sign << 31) | 0x7f800000 | (frac << 13);
    else
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (frac << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct RknnEngine::Impl {
    rknn_context ctx = 0;
    bool loaded = false;
    rknn_input_output_num io{};
    std::vector<rknn_tensor_attr> out_attrs;
    // 输出暂存（want_float=0 → 手动反量化）；swap 进 outputs 零拷贝复用
    std::vector<std::vector<float>> outs_f;
};

RknnEngine::RknnEngine() : impl_(std::make_unique<Impl>()) {}

RknnEngine::~RknnEngine() {
    if (impl_->loaded) rknn_destroy(impl_->ctx);
}

bool RknnEngine::load(const std::string& model_path) {
    FILE* f = fopen(model_path.c_str(), "rb");
    if (!f) {
        err_ = "open model fail: " + model_path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz > 0 ? static_cast<size_t>(sz) : 0);
    size_t rd = fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (rd != data.size() || data.empty()) {
        err_ = "read model fail: " + model_path;
        return false;
    }

    if (rknn_init(&impl_->ctx, data.data(), sz, 0, nullptr) < 0) {
        err_ = "rknn_init fail: " + model_path;
        return false;
    }

    if (rknn_query(impl_->ctx, RKNN_QUERY_IN_OUT_NUM, &impl_->io, sizeof(impl_->io)) < 0) {
        err_ = "rknn_query IN_OUT_NUM fail";
        return false;
    }
    if (impl_->io.n_input != 1 || impl_->io.n_output != 9) {
        err_ = "模型不是 zoo 9 输出版: inputs=" + std::to_string(impl_->io.n_input) +
               " outputs=" + std::to_string(impl_->io.n_output);
        return false;
    }

    // K10：query 前设置 .index=i（官方 yolov8.cc 同款；否则全部查到第 0 个张量）
    impl_->out_attrs.resize(impl_->io.n_output);
    impl_->outs_f.resize(impl_->io.n_output);
    for (uint32_t i = 0; i < impl_->io.n_output; ++i) {
        impl_->out_attrs[i].index = i;
        if (rknn_query(impl_->ctx, RKNN_QUERY_OUTPUT_ATTR, &impl_->out_attrs[i],
                       sizeof(rknn_tensor_attr)) < 0) {
            err_ = "rknn_query OUTPUT_ATTR[" + std::to_string(i) + "] fail";
            return false;
        }
        impl_->outs_f[i].resize(impl_->out_attrs[i].n_elems);
        // zoo 输出 NCHW：[1,C,H,W] → dims[2]=H, dims[3]=W（YoloPostConfig 用）
        out_hw_.emplace_back(impl_->out_attrs[i].dims[2], impl_->out_attrs[i].dims[3]);
    }

    rknn_tensor_attr in_attr{};
    in_attr.index = 0;  // K10
    if (rknn_query(impl_->ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) < 0) {
        err_ = "rknn_query INPUT_ATTR fail";
        return false;
    }
    // zoo 输入 NHWC RGB（dims[1]/[2]=H/W 值序因驱动版本而异，宽高相等场景无歧义；
    // manifest 为准，此处仅用于与 manifest 交叉校验）
    in_w_ = in_attr.dims[1];
    in_h_ = in_attr.dims[2];

    // 板上 librknnrt 版本无 rknn_context_mem_size / RKNN_QUERY_MEM_SIZE，
    // mem_kb() 恒 0（接口保留）；需内存占用时改用 /proc/<pid>/s 统计

    impl_->loaded = true;
    return true;
}

bool RknnEngine::set_core(Core c) {
    return set_core_index(c == Core::Steady ? 0 : 1);
}

bool RknnEngine::set_core_index(int core_id) {
    if (!impl_->loaded) {
        err_ = "set_core before load";
        return false;
    }
    // K11：显式绑核。AUTO = 驱动随机单核，破坏 steady/triggered 分区纪律（架构 §7.3）。
    // 仅单核掩码：多核协同（0_1/0_1_2）单次推理独占全 NPU，与 NpuCorePlan 分区互斥，禁止
    rknn_core_mask mask;
    switch (core_id) {
        case 0: mask = RKNN_NPU_CORE_0; break;
        case 1: mask = RKNN_NPU_CORE_1; break;
        case 2: mask = RKNN_NPU_CORE_2; break;
        default:
            err_ = "core id 越界（仅 0/1/2 单核）: " + std::to_string(core_id);
            return false;
    }
    if (rknn_set_core_mask(impl_->ctx, mask) < 0) {
        err_ = "rknn_set_core_mask fail";
        return false;
    }
    return true;
}

bool RknnEngine::run(const InferTensor& in, std::vector<std::vector<float>>& outputs) {
    if (!impl_->loaded) {
        err_ = "run before load";
        return false;
    }
    if (in.data.empty()) {
        err_ = "empty input tensor";
        return false;
    }

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;  // 0~255，运行时按模型量化参数处理
    input.size = in.data.size();     // w*h*3（NHWC RGB888）
    input.fmt = RKNN_TENSOR_NHWC;    // zoo 模型实测输入格式
    input.buf = const_cast<uint8_t*>(in.data.data());
    if (rknn_inputs_set(impl_->ctx, 1, &input) < 0) {
        err_ = "rknn_inputs_set fail";
        return false;
    }

    // K12 口径：只测 rknn_run（纯 NPU 段）
    uint64_t t0 = now_us();
    if (rknn_run(impl_->ctx, nullptr) < 0) {
        err_ = "rknn_run fail";
        return false;
    }
    last_run_us_.store(now_us() - t0, std::memory_order_relaxed);

    std::vector<rknn_output> raw(impl_->io.n_output);
    for (uint32_t i = 0; i < impl_->io.n_output; ++i) {
        raw[i].index = i;           // K10
        raw[i].want_float = 0;      // 原始量化输出，手动反量化
    }
    if (rknn_outputs_get(impl_->ctx, impl_->io.n_output, raw.data(), nullptr) < 0) {
        err_ = "rknn_outputs_get fail";
        return false;
    }

    // 反量化分派（INT8 affine / FP16 / float 兜底）；swap 复用避免每帧 1.6MB 拷贝
    outputs.resize(impl_->io.n_output);
    for (uint32_t i = 0; i < impl_->io.n_output; ++i) {
        const auto& a = impl_->out_attrs[i];
        auto& o = impl_->outs_f[i];
        // swap 复用的回流纪律：上轮 swap 后 outs_f[i] 持有的是调用方旧容器
        //（首轮为空），直接 o[k]= 会写空指针——第二轮 run 必崩（板上 gdb 实证）。
        // 每轮先重铺到 attr 尺寸，异径旧容器同样被纠正，之后才是零拷贝交替复用
        if (o.size() != a.n_elems) o.resize(a.n_elems);
        if (a.type == RKNN_TENSOR_INT8) {
            const int8_t* p = static_cast<const int8_t*>(raw[i].buf);
            for (uint32_t k = 0; k < a.n_elems; ++k) o[k] = (p[k] - a.zp) * a.scale;
        } else if (a.type == RKNN_TENSOR_FLOAT16) {
            const uint16_t* p = static_cast<const uint16_t*>(raw[i].buf);
            for (uint32_t k = 0; k < a.n_elems; ++k) o[k] = fp16_to_f32(p[k]);
        } else {
            const float* p = static_cast<const float*>(raw[i].buf);
            for (uint32_t k = 0; k < a.n_elems; ++k) o[k] = p[k];
        }
        rknn_outputs_release(impl_->ctx, 1, &raw[i]);
        outputs[i].swap(o);  // 下次 run 时 o 为空容器，重新填充
    }
    return true;
}

}  // namespace aibox
