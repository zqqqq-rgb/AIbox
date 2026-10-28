#include "hw/RtspPuller.h"

#include "common/Log.h"

// FFmpeg 是 C 库，必须 extern "C" 包裹（demo K 坑：板上 avformat.h 缺 extern "C" 保护，
// 否则符号被 C++ mangle → 链接 undefined reference）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>

namespace aibox {

namespace {

std::once_flag g_av_init;
void av_init_once() { avformat_network_init(); }

// interrupt_callback：close() 置 abort → 阻塞中的 avformat_open_input / av_read_frame
// 立即返回错误（看门狗跨线程解锁的唯一手段）
int interrupt_cb(void* opaque) {
    auto* f = static_cast<std::atomic<bool>*>(opaque);
    return f->load(std::memory_order_relaxed) ? 1 : 0;
}

// ---- AVCC / HVCC → Annex-B（demo avcc_to_annexb 泛化为 H.264/H.265 两种 extradata）----
void append_nalu(std::vector<uint8_t>& out, const uint8_t* d, size_t n) {
    const uint8_t sc[4] = {0, 0, 0, 1};
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), d, d + n);
}

bool is_annexb_start(const uint8_t* d, size_t n) {
    return n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1;
}

std::vector<uint8_t> extradata_to_annexb(const uint8_t* d, size_t n, RtspPuller::Codec codec) {
    std::vector<uint8_t> out;
    if (!d || n == 0) return out;
    if (is_annexb_start(d, n)) {  // 已是 Annex-B，原样
        out.assign(d, d + n);
        return out;
    }
    if (codec == RtspPuller::Codec::H264) {
        // AVCC (ISO 14496-15)：[0]=1 [4]=0xFC|lenSize [5]=0xE0|numSPS {[2B len][SPS]} numPPS...
        if (n < 7 || d[0] != 1) { out.assign(d, d + n); return out; }
        size_t p = 5;
        int num_sps = d[p] & 0x1f;
        ++p;
        for (int i = 0; i < num_sps && p + 2 <= n; ++i) {
            uint16_t len = static_cast<uint16_t>((d[p] << 8) | d[p + 1]);
            p += 2;
            if (p + len > n) break;
            append_nalu(out, d + p, len);
            p += len;
        }
        if (p >= n) return out;
        int num_pps = d[p];
        ++p;
        for (int i = 0; i < num_pps && p + 2 <= n; ++i) {
            uint16_t len = static_cast<uint16_t>((d[p] << 8) | d[p + 1]);
            p += 2;
            if (p + len > n) break;
            append_nalu(out, d + p, len);
            p += len;
        }
    } else {
        // HVCC (ISO 14496-15 §8.3.3)：[22]=numOfArrays；
        // 每数组 [1B type|complete] [2B numNalus] {[2B len][NALU]}...
        if (n < 23 || d[0] != 1) { out.assign(d, d + n); return out; }
        size_t p = 22;
        int num_arrays = d[p];
        ++p;
        for (int a = 0; a < num_arrays && p + 3 <= n; ++a) {
            ++p;  // 1B: array_completeness | NAL_unit_type
            int num = (d[p] << 8) | d[p + 1];
            p += 2;
            for (int i = 0; i < num && p + 2 <= n; ++i) {
                uint16_t len = static_cast<uint16_t>((d[p] << 8) | d[p + 1]);
                p += 2;
                if (p + len > n) return out;
                append_nalu(out, d + p, len);
                p += len;
            }
        }
    }
    return out;
}

}  // namespace

RtspPuller::RtspPuller(std::string url, uint32_t connect_timeout_s)
    : url_(std::move(url)), connect_timeout_s_(connect_timeout_s) {}

RtspPuller::~RtspPuller() { close(); }

bool RtspPuller::open() {
    std::call_once(g_av_init, av_init_once);

    std::lock_guard<std::mutex> lk(mu_);
    cleanup_locked();
    abort_.store(false, std::memory_order_relaxed);
    pts_base_ = -1;
    sps_pending_ = false;
    annexb_.clear();

    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = &abort_;
    fmt->max_analyze_duration = 1 * AV_TIME_BASE;  // 压低 SDP 协商时长（低延迟）

    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "rtsp_transport", "tcp", 0);
    av_dict_set(&opt, "stimeout", "5000000", 0);  // RTSP 层 5s 看门狗（demo 同款）
    av_dict_set(&opt, "max_delay", "100000", 0);  // 100ms（回环抖动缓冲压低，架构 §9）
    // 连接超时兜底：interrupt 回调只响应 close()；此处由 connect_timeout_s 后的
    // 看门狗/调用方兜底，ffmpeg 自身对 TCP connect 无统一超时参数

    int ret = avformat_open_input(&fmt, url_.c_str(), nullptr, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        char err[128] = {0};
        av_strerror(ret, err, sizeof(err));
        LOGE("pull", "open fail(%s): %s", url_.c_str(), err);
        if (fmt) avformat_free_context(fmt);
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOGE("pull", "find_stream_info fail: %s", url_.c_str());
        avformat_close_input(&fmt);
        return false;
    }

    vidx_ = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx_ < 0) {
        LOGE("pull", "no video stream: %s", url_.c_str());
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecParameters* par = fmt->streams[vidx_]->codecpar;
    codec_ = (par->codec_id == AV_CODEC_ID_HEVC) ? Codec::H265 : Codec::H264;

    // SPS/PPS（SDP extradata）→ Annex-B 首包（K1：硬解启动前提）
    if (par->extradata && par->extradata_size > 0) {
        annexb_ = extradata_to_annexb(par->extradata,
                                      static_cast<size_t>(par->extradata_size), codec_);
        sps_pending_ = !annexb_.empty();
    }

    fmt_ = fmt;
    LOGI("pull", "connected %s stream#%d codec=%s extradata=%d",
         url_.c_str(), vidx_,
         codec_ == Codec::H265 ? "h265" : "h264",
         par->extradata_size);
    return true;
}

bool RtspPuller::read(Packet& out) {
    reading_.store(true, std::memory_order_release);

    // 借用语义：out.data 指向成员缓冲，有效至下一次 read（IDecoder 在本次调用内消费）
    if (sps_pending_) {
        sps_pending_ = false;
        out.data = annexb_.data();
        out.size = static_cast<uint32_t>(annexb_.size());
        out.pts_ms = 0;
        reading_.store(false, std::memory_order_release);
        return true;
    }

    AVFormatContext* fmt = nullptr;
    int vidx = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fmt = fmt_;
        vidx = vidx_;
    }
    if (!fmt || abort_.load(std::memory_order_relaxed)) {
        reading_.store(false, std::memory_order_release);
        return false;
    }

    AVPacket* pkt = av_packet_alloc();  // 官方分配器（demo K 坑：栈上未初始化 AVPacket 有风险）
    bool ok = false;
    // 循环跳过非视频包（音频/数据）——子码流带 AAC 音轨时，音频包若直接返回 false
    // 会被 src_loop 当断流 → 重连风暴（板上实测：~1s/次 connected 刷屏、零解码帧）
    while (pkt) {
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index == vidx && pkt->size > 0) {
            // pts：流时基 → ms，会话首帧归零（回绕/跳变钳 0，摄像头重启保护）
            AVRational tb = fmt->streams[vidx]->time_base;
            int64_t pts_ms = (pkt->pts != AV_NOPTS_VALUE)
                                 ? av_rescale_q(pkt->pts, tb, AVRational{1, 1000})
                                 : 0;
            if (pts_base_ < 0) pts_base_ = pts_ms;
            int64_t rel = pts_ms - pts_base_;
            if (rel < 0) rel = 0;

            pkt_buf_.assign(pkt->data, pkt->data + pkt->size);
            out.data = pkt_buf_.data();
            out.size = static_cast<uint32_t>(pkt_buf_.size());
            out.pts_ms = static_cast<uint64_t>(rel);
            ok = true;
            break;
        }
        av_packet_unref(pkt);  // 音频/数据包：丢弃，继续取下一个
    }
    av_packet_free(&pkt);
    reading_.store(false, std::memory_order_release);
    return ok;
}

void RtspPuller::close() {
    abort_.store(true, std::memory_order_relaxed);
    // 等待在飞的 read 退出（interrupt_callback 保证 av_read_frame 数 ms 内返回），
    // 之后才能 avformat_close_input（防 UAF）。上限 100ms 兜底
    for (int i = 0; i < 100 && reading_.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::lock_guard<std::mutex> lk(mu_);
    cleanup_locked();
    abort_.store(false, std::memory_order_relaxed);  // 复位，下次 open 正常
}

void RtspPuller::cleanup_locked() {
    if (fmt_) {
        avformat_close_input(&fmt_);
        fmt_ = nullptr;
    }
    vidx_ = -1;
    sps_pending_ = false;
    annexb_.clear();
}

}  // namespace aibox
