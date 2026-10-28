#include "hw/RgaPreproc.h"

#include "common/Log.h"

#include <cstddef>  // 板上 librga 头（im2d_single.h）C++ 编译缺 NULL，须先于此引入
#include <im2d.h>
#include <rga.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "common/LetterboxMath.h"

namespace aibox {

namespace {
std::atomic<bool> g_zerocopy_unknown{true};  // 降级告警只打一次
}

bool RgaPreproc::run(const Frame& in, InferTensor& out, LetterBoxParam& param) {
    const uint32_t dw = out.width, dh = out.height;
    if (in.width == 0 || in.height == 0 || dw == 0 || dh == 0 || out.data.empty())
        return false;

    // 参数与 SoftLetterbox 同源（黄金对照的第一前提）
    param = compute_letterbox(in.width, in.height, dw, dh);

    // padding 先填 0（与 SoftLetterbox 一致，逐像素对照口径）
    std::memset(out.data.data(), 0, out.data.size());

    rga_buffer_t dst =
        wrapbuffer_virtualaddr(out.data.data(), dw, dh, RK_FORMAT_RGB_888);

    // 有效区矩形（模型输入坐标系内）
    im_rect dst_rect;
    dst_rect.x = static_cast<int>(param.pad_x);
    dst_rect.y = static_cast<int>(param.pad_y);
    dst_rect.width = static_cast<int>(static_cast<float>(in.width) * param.scale);
    dst_rect.height = static_cast<int>(static_cast<float>(in.height) * param.scale);

    // NV12 总行数（UV 平面偏移 = wstride * hstride）：Frame.vstride_y=0 表示紧凑
    const uint32_t wstride =
        in.stride_y ? in.stride_y : in.width;
    const uint32_t hstride = in.vstride_y ? in.vstride_y : in.height;

    // 方式 A：dma-buf fd 直通（零拷贝）；im2d 内部锁串行化（架构 §16）
    if (in.y_fd >= 0) {
        // _t 系列签名 = (fd, w, h, wstride, hstride, format)——format 在最后；
        // format 误放第 4 位会以 hstride 充当格式号（0x1e0=480，板上实测报错）
        rga_buffer_t src =
            wrapbuffer_fd_t(in.y_fd, in.width, in.height,
                            static_cast<int>(wstride), static_cast<int>(hstride),
                            RK_FORMAT_YCbCr_420_SP);
        IM_STATUS st = improcess(src, dst, {}, {}, dst_rect, {}, IM_SYNC);
        if (st > 0) return true;  // K9：成功 = IM_STATUS_SUCCESS(1)/NOERROR(2)，判据 ret<=0

        if (g_zerocopy_unknown.exchange(false))
            LOGW("rga", "fd 路径失败(%s)，降级内存路径", imStrError(st));
    }

    // 方式 B：虚拟地址（保险路径；UV 平面按 vstride 偏移——MPP 帧的 y_ptr 是整帧基址）
    if (!in.y_ptr) return false;
    rga_buffer_t src =
        wrapbuffer_virtualaddr_t(const_cast<uint8_t*>(in.y_ptr), in.width, in.height,
                                 static_cast<int>(wstride), static_cast<int>(hstride),
                                 RK_FORMAT_YCbCr_420_SP);
    IM_STATUS st = improcess(src, dst, {}, {}, dst_rect, {}, IM_SYNC);
    if (st <= 0) {  // K9
        LOGE("rga", "improcess failed: %s", imStrError(st));
        return false;
    }
    return true;
}

}  // namespace aibox
