// rga_golden —— RGA 硬件 letterbox 与 CPU 黄金实现的逐帧对照工具（任务分解 T3.2）
//
// 对照口径（任务分解 §M3-T3.2）：
//   1. 几何参数：LetterboxMath 同源生成，RGA 与 Soft 的 scale/pad 差 ≤0.5px = 通过
//   2. 像素差：逐像素 |A-B| 统计 max/mean/超阈值率（YUV→RGB 矩阵与插值器差异
//      属硬件实现域，报告判读；几何不过 = 硬失败退出码 1）
// 用法：
//   rga_golden --frame f.nv12 --w 640 --h 480 [--model-w 640 --model-h 640]
// 可在板上重复执行（DoD）；fd 直通路径（零拷贝）由 --dma 开关补充验证：
//   rga_golden --frame f.nv12 --w 640 --h 480 --dma   （经 dma_heap 分配源帧）
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/DmaBuffer.h"
#include "common/LetterboxMath.h"
#include "core/fakes/soft_letterbox.h"
#include "hw/DmaHeapAllocator.h"
#include "hw/RgaPreproc.h"

using namespace aibox;

namespace {
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

int main(int argc, char** argv) {
    std::string frame_path;
    uint32_t w = 640, h = 480, mw = 640, mh = 640;
    bool use_dma = false;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--frame") frame_path = next();
        else if (s == "--w") w = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--h") h = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--model-w") mw = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--model-h") mh = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (s == "--dma") use_dma = true;
        else {
            std::fprintf(stderr,
                         "用法: rga_golden --frame f.nv12 --w W --h H "
                         "[--model-w W --model-h H] [--dma]\n");
            return 2;
        }
    }
    if (frame_path.empty()) {
        std::fprintf(stderr, "--frame 必填\n");
        return 2;
    }

    // 读 NV12 帧（紧凑布局；MPP 真帧带对齐补行的场景由集成自测覆盖，此处对照
    // Soft/RGA 同一输入即可达成"参数同源 + 像素可比"目的）
    const size_t yuv_bytes = static_cast<size_t>(w) * h * 3 / 2;
    std::vector<uint8_t> yuv(yuv_bytes);
    FILE* f = fopen(frame_path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "open %s fail\n", frame_path.c_str());
        return 2;
    }
    size_t rd = fread(yuv.data(), 1, yuv_bytes, f);
    fclose(f);
    if (rd != yuv_bytes) {
        std::fprintf(stderr, "短读: %zu/%zu（--w/--h 与文件不符？）\n", rd, yuv_bytes);
        return 2;
    }

    Frame src;
    src.pts_ms = now_ms();
    src.width = w;
    src.height = h;
    src.stride_y = w;
    src.stride_uv = w;
    src.vstride_y = h;
    src.y_ptr = yuv.data();
    src.uv_ptr = yuv.data() + static_cast<size_t>(w) * h;

    if (use_dma) {
        // fd 直通路径：源帧入 dma_heap（真实 dma-buf），RGA 走方式 A（零拷贝）
        DmaHeapAllocator alloc;
        Slab slab = alloc.allocate(w, h, kDrmFmtNV12);
        if (slab.fd < 0) {
            std::fprintf(stderr, "dma_heap 分配失败\n");
            return 2;
        }
        std::memcpy(slab.map, yuv.data(), yuv_bytes);
        src.y_fd = slab.fd;
        src.uv_fd = -1;  // NV12 单 fd，UV 偏移由 RGA 按 wstride*hstride 解析

        InferTensor out, ref;
        out.width = ref.width = mw;
        out.height = ref.height = mh;
        out.data.resize(static_cast<size_t>(mw) * mh * 3);
        ref.data.resize(static_cast<size_t>(mw) * mh * 3);
        LetterBoxParam pa, pb;
        RgaPreproc rga;
        SoftLetterbox soft;
        bool ok_a = rga.run(src, out, pa);
        bool ok_b = soft.run(src, ref, pb);
        if (!ok_a || !ok_b) {
            std::fprintf(stderr, "run fail: rga=%d soft=%d\n", ok_a, ok_b);
            alloc.deallocate(slab);
            return 1;
        }
        double max_diff = 0, sum = 0;
        size_t mismatch = 0;
        for (size_t i = 0; i < out.data.size(); ++i) {
            int d = std::abs(static_cast<int>(out.data[i]) - static_cast<int>(ref.data[i]));
            max_diff = std::max(max_diff, static_cast<double>(d));
            sum += d;
            if (d > 8) ++mismatch;
        }
        std::printf("[dma 路径] scale rga=%.4f soft=%.4f pad rga=(%.1f,%.1f) soft=(%.1f,%.1f)\n",
                    pa.scale, pb.scale, pa.pad_x, pa.pad_y, pb.pad_x, pb.pad_y);
        std::printf("[dma 路径] 像素差: max=%.0f mean=%.2f mismatch(>8)=%zu/%zu\n",
                    max_diff, sum / out.data.size(), mismatch, out.data.size());
        alloc.deallocate(slab);
        return 0;
    }

    // 常规对照：指针路径
    InferTensor out, ref;
    out.width = ref.width = mw;
    out.height = ref.height = mh;
    out.data.resize(static_cast<size_t>(mw) * mh * 3);
    ref.data.resize(static_cast<size_t>(mw) * mh * 3);
    LetterBoxParam pa, pb;

    RgaPreproc rga;
    SoftLetterbox soft;
    bool ok_a = rga.run(src, out, pa);
    bool ok_b = soft.run(src, ref, pb);
    if (!ok_a || !ok_b) {
        std::fprintf(stderr, "run fail: rga=%d soft=%d\n", ok_a, ok_b);
        return 1;
    }

    // 几何判据（>0.5px = 失败）：scale 相对差 × 模型边长 ≤0.5px 等效换算
    double dx = std::abs(static_cast<double>(pa.pad_x) - pb.pad_x);
    double dy = std::abs(static_cast<double>(pa.pad_y) - pb.pad_y);
    double ds = std::abs(static_cast<double>(pa.scale) - pb.scale) * mw;
    bool geo_ok = dx <= 0.5 && dy <= 0.5 && ds <= 0.5;

    double max_diff = 0, sum = 0;
    size_t mismatch = 0;
    for (size_t i = 0; i < out.data.size(); ++i) {
        int d = std::abs(static_cast<int>(out.data[i]) - static_cast<int>(ref.data[i]));
        max_diff = std::max(max_diff, static_cast<double>(d));
        sum += d;
        if (d > 8) ++mismatch;
    }

    std::printf("几何: scale rga=%.5f soft=%.5f | pad rga=(%.2f,%.2f) soft=(%.2f,%.2f)\n",
                pa.scale, pb.scale, pa.pad_x, pa.pad_y, pb.pad_x, pb.pad_y);
    std::printf("像素: max=%.0f mean=%.2f mismatch(>8)=%zu / %zu (%.3f%%)\n", max_diff,
                sum / out.data.size(), mismatch, out.data.size(),
                100.0 * mismatch / out.data.size());
    std::printf("%s\n",
                geo_ok ? "[PASS] 几何差 ≤0.5px（黄金对照通过）"
                       : "[FAIL] 几何差 >0.5px（黄金对照失败）");
    return geo_ok ? 0 : 1;
}
