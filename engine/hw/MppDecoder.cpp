#include "hw/MppDecoder.h"

#include "common/Log.h"

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <thread>
#include <utility>

namespace aibox {

struct MppDecoder::Impl {
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup fr_grp = nullptr;   // 帧 buffer 组（info-change 时挂载，K4）
    MppCodingType coding = MPP_VIDEO_CodingAVC;

    // 已解出待取的帧（同一次 decode 可产出多帧；携带 MppFrame 引用保 fd 生命周期）
    struct Pending {
        Frame f;
        MppFrame mf = nullptr;
    };
    std::deque<Pending> pending_;
    MppFrame held_ = nullptr;  // 上一次 decode 返回的帧（借用期保活，下次 decode 释放）

    std::atomic<uint64_t> frames_out_{0};
    uint32_t err_frames_ = 0;  // 不可解码帧累计（日志防刷屏：首条+每100条报一次）

    void release_held() {
        if (held_) {
            mpp_frame_deinit(&held_);
            held_ = nullptr;
        }
    }

    // ---- 输出拉取（K2：MPP_OK+NULL 必须 break；K4：info-change 挂组）----
    int pull_frames() {
        int got = 0;
        while (true) {
            MppFrame frame = nullptr;
            if (mpi->decode_get_frame(ctx, &frame) != MPP_OK) break;
            if (frame == nullptr) break;  // K2：非阻塞无帧 → 返回继续喂包

            if (mpp_frame_get_info_change(frame)) {
                // K4：官方流程——用 info-change 帧携带的 buf_size 挂载帧 buffer 组
                MppBufferGroup grp = nullptr;
                MPP_RET ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
                if (ret != MPP_OK) {
                    // DRM 失败退化普通内存（fd 无效，只走 CPU 指针路径）
                    LOGE("mpp", "DRM buf fail(%d), fallback malloc", ret);
                    ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_NORMAL);
                    if (ret != MPP_OK) {
                        mpp_frame_deinit(&frame);
                        break;
                    }
                }
                ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
                if (ret != MPP_OK) {
                    LOGW("mpp", "set ext bufgrp warn %d", ret);
                    mpp_buffer_group_put(grp);
                } else {
                    MppBufferGroup old = fr_grp;  // 新组生效后再释放旧组
                    fr_grp = grp;
                    if (old) mpp_buffer_group_put(old);
                }
                mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
                LOGI("mpp", "info change -> %ux%u",
                     mpp_frame_get_width(frame), mpp_frame_get_height(frame));
                mpp_frame_deinit(&frame);
                continue;
            }
            if (mpp_frame_get_eos(frame)) {
                mpp_frame_deinit(&frame);
                break;
            }
            if (mpp_frame_get_errinfo(frame)) {
                // 0x10 = FRAME_ERR_NOT_DECODABLE：缺参考帧（中途入流未到 IDR 属正常，
                // 下一 GOP 自动同步）；全量打印会以 25fps 刷屏，仅首条+每 100 条上报
                ++err_frames_;
                if (err_frames_ == 1 || (err_frames_ % 100) == 0)
                    LOGW("mpp", "frame err 0x%x (n=%u, 等待IDR同步)",
                         mpp_frame_get_errinfo(frame), err_frames_);
                mpp_frame_deinit(&frame);
                continue;
            }

            MppBuffer buf = mpp_frame_get_buffer(frame);
            if (buf) {
                const uint32_t fw = mpp_frame_get_width(frame);
                const uint32_t fh = mpp_frame_get_height(frame);
                const uint32_t hs = mpp_frame_get_hor_stride(frame);
                // 防御性拦截：宽高为 0/奇数、stride<width 的帧一律丢弃。
                //（v1 曾误诊为"MPP 重载元数据损坏"，真因是 remove_cam 清理期
                // FramePool UAF——已在 FramePool 共享 State 修复；此守卫保留作
                // 纵深防御，真损坏帧也不应流入下游）
                if (fw == 0 || fh == 0 || (fw & 1) || (fh & 1) || hs < fw ||
                    mpp_frame_get_ver_stride(frame) < fh) {
                    ++err_frames_;
                    if (err_frames_ == 1 || (err_frames_ % 100) == 0)
                        LOGW("mpp", "丢弃损坏帧 %ux%u(stride %u) (n=%u)",
                             fw, fh, hs, err_frames_);
                    mpp_frame_deinit(&frame);
                    continue;
                }
                Pending p;
                p.mf = frame;  // 引用保活，Frame 填借用指针
                Frame& f = p.f;
                f.width = fw;
                f.height = fh;
                f.format = PixelFormat::kNV12;
                f.stride_y = hs;
                f.stride_uv = hs;
                f.vstride_y = mpp_frame_get_ver_stride(frame);
                f.pts_ms = static_cast<uint64_t>(mpp_frame_get_pts(frame));
                f.y_fd = mpp_buffer_get_fd(buf);  // 单 dma-buf 承载 NV12（Y+UV 连续）
                f.uv_fd = f.y_fd;
                auto* base = static_cast<const uint8_t*>(mpp_buffer_get_ptr(buf));
                f.y_ptr = base;
                f.uv_ptr = base + static_cast<size_t>(f.stride_y) * f.vstride_y;
                frames_out_.fetch_add(1, std::memory_order_relaxed);
                pending_.push_back(std::move(p));
                got++;
                continue;  // frame 所有权移交 pending_，不 deinit
            }
            mpp_frame_deinit(&frame);
        }
        return got;
    }

    // ---- K3：put 失败(BUFFER_FULL) → 排空输出（唤醒解析线程应答 info-change）→ 重试
    MPP_RET put_packet_drain(MppPacket pkt) {
        MPP_RET ret = MPP_NOK;
        for (int retry = 0; retry < 200; ++retry) {  // 200*2ms=0.4s 兜底
            ret = mpi->decode_put_packet(ctx, pkt);
            if (ret == MPP_OK || ret != MPP_ERR_BUFFER_FULL) return ret;
            pull_frames();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return ret;
    }
};

MppDecoder::MppDecoder() = default;
MppDecoder::~MppDecoder() = default;

bool MppDecoder::open(uint32_t width, uint32_t height) {
    return open(width, height, Codec::H264);
}

bool MppDecoder::open(uint32_t width, uint32_t height, Codec codec) {
    (void)width;
    (void)height;  // H.264/H.265 分辨率由 SPS 决定
    impl_ = std::make_unique<Impl>();
    impl_->coding =
        (codec == Codec::H265) ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;

    MPP_RET ret = mpp_create(&impl_->ctx, &impl_->mpi);
    if (ret != MPP_OK) {
        LOGE("mpp", "mpp_create fail %d", ret);
        impl_.reset();
        return false;
    }

    // K6：低延迟参数须在 mpp_init 前（rk_mpi_cmd.h "Need to setup before init"）
    int fast = 1;
    impl_->mpi->control(impl_->ctx, MPP_DEC_SET_PARSER_FAST_MODE, &fast);
    impl_->mpi->control(impl_->ctx, MPP_DEC_SET_IMMEDIATE_OUT, &fast);

    ret = mpp_init(impl_->ctx, MPP_CTX_DEC, impl_->coding);
    if (ret != MPP_OK) {
        LOGE("mpp", "mpp_init fail %d", ret);
        mpp_destroy(impl_->ctx);
        impl_.reset();
        return false;
    }

    // K5：0 = 非阻塞（get_frame 无帧立即返回 NULL）
    int64_t in_timeout = 0;
    int64_t out_timeout = 0;
    impl_->mpi->control(impl_->ctx, MPP_SET_INPUT_TIMEOUT, &in_timeout);
    impl_->mpi->control(impl_->ctx, MPP_SET_OUTPUT_TIMEOUT, &out_timeout);

    LOGI("mpp", "decoder open (%s, fast-parse + immediate-out)",
         codec == Codec::H265 ? "h265" : "h264");
    return true;
}

bool MppDecoder::decode(const Packet& in, Frame& out) {
    if (!impl_ || !impl_->ctx) return false;

    // 借用语义：上一次返回的 Frame（MPP 内部 buffer）自此失效
    impl_->release_held();

    // 先出积压帧（同一 feed 产出的多帧按序返回，不重复喂包）
    if (!impl_->pending_.empty()) {
        out = std::move(impl_->pending_.front().f);
        impl_->held_ = impl_->pending_.front().mf;
        impl_->pending_.pop_front();
        return true;
    }

    // 喂一包（数据借用语义：MPP packet 引用 in.data，put 后由 MPP 拷入内部任务）
    MppPacket pkt = nullptr;
    if (mpp_packet_init(&pkt, const_cast<void*>(static_cast<const void*>(in.data)),
                        in.size) != MPP_OK)
        return false;
    mpp_packet_set_pts(pkt, static_cast<int64_t>(in.pts_ms));
    MPP_RET ret = impl_->put_packet_drain(pkt);
    mpp_packet_deinit(&pkt);
    if (ret != MPP_OK) {
        LOGE("mpp", "put_packet err %d", ret);
        return false;
    }

    impl_->pull_frames();  // 同步拉帧（喂包驱动解码器，K2/K4 在此消化）

    if (impl_->pending_.empty()) return false;  // 本包未产出帧（如 SPS/PPS/非参考帧）
    out = std::move(impl_->pending_.front().f);
    impl_->held_ = impl_->pending_.front().mf;
    impl_->pending_.pop_front();
    return true;
}

void MppDecoder::close() {
    if (!impl_) return;
    impl_->release_held();
    while (!impl_->pending_.empty()) {  // 积压帧全部释放
        MppFrame mf = impl_->pending_.front().mf;
        if (mf) mpp_frame_deinit(&mf);
        impl_->pending_.pop_front();
    }
    // 释放顺序（demo review 修复）：reset(归还内部 buffer) → 解绑+put 外部组 → destroy
    if (impl_->ctx) {
        impl_->mpi->reset(impl_->ctx);
        if (impl_->fr_grp) {
            impl_->mpi->control(impl_->ctx, MPP_DEC_SET_EXT_BUF_GROUP, nullptr);
            mpp_buffer_group_put(impl_->fr_grp);
            impl_->fr_grp = nullptr;
        }
        mpp_destroy(impl_->ctx);
        impl_->ctx = nullptr;
    }
    impl_.reset();
}

}  // namespace aibox
