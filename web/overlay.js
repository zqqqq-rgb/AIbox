// overlay.js —— 元数据叠加 DOM 薄壳（任务分解 T5.1，详设 §6.3）
// 全部对齐/外推/淘汰/绘制计算委托 overlay_core.js 纯函数；本文件只做：
//   DOM 提取（currentTime / buffered / canvas 尺寸）→ 调纯函数 → 照抄执行绘制指令
//
// 用法（宫格每 Tile 一个实例，单路放大另建）：
//   const ov = new MetaOverlay(video, canvas, { mode: 'webrtc', classes: [...] });
//   video.addEventListener('canplay', () => ov.onVideoReady());   // 首帧就绪
//   ws.onmessage = m => ov.onMeta(JSON.parse(m.data));            // /ws/meta/{cam}
//   ov.start(); ... ov.stop();
//   player 重连 / WS 重连后调 ov.reset()（清状态重新锚定）
import * as core from './overlay_core.js';

export class MetaOverlay {
    constructor(video, canvas, opts = {}) {
        this.video = video;
        this.canvas = canvas;
        this.ctx = canvas.getContext('2d');
        this.mode = opts.mode || 'webrtc';        // 'webrtc' | 'mse'
        this.classes = opts.classes || [];

        // 核心状态（全部由 overlay_core 纯函数转换）
        this.anchorFp = null;                     // pts 零点锚（us）；null=未锚
        this.detQ = [];                           // 检测队列 [{fp, objs}]
        this.lastObjs = null;                     // 最近一次对齐命中的目标集
        this.videoReady = false;
        this.detW = 0; this.detH = 0;             // 检测域分辨率（frame_w/frame_h）
        this.rois = [];                           // 已保存 ROI（归一化顶点数组，静态层）
        this._raf = 0;
    }

    // 视频首帧就绪（canplay/onPlaying）——锚定前置条件
    onVideoReady() { this.videoReady = true; }

    // 已保存 ROI（listCameras 回显：一通道多算法多块；元素 {pts, color}——
    // color 为配置页颜色卡选色，空 = 默认主题色）
    setRois(rois) { this.rois = Array.isArray(rois) ? rois : []; }

    // 播放器重建 / WS 重连后：清状态重新锚定（锚定只发生一次，锚后不变）
    reset() {
        this.anchorFp = null;
        this.detQ = [];
        this.lastObjs = null;
    }

    // WS /ws/meta/{cam} 每条 detections（D3 后含 frame_w/frame_h）
    onMeta(msg) {
        if (!msg) return;
        const fp = typeof msg.frame_pts === 'number' ? msg.frame_pts : 0;
        // 检测域分辨率：D3 契约字段为准（主码流放大时 ≠ video.videoWidth）
        if (msg.frame_w > 0 && msg.frame_h > 0) {
            this.detW = msg.frame_w;
            this.detH = msg.frame_h;
        }
        // ① 零点锚定（仅一次）：computeAnchor 对已有锚原样返回（锚后不再变）
        if (this.anchorFp === null && this.videoReady) {
            this.anchorFp = core.computeAnchor(
                null, this.mode, fp, this._videoEdgeSec());
        }
        // ② 入队（容量 64 丢最旧）——纯函数
        // 框位置策略：检测在哪就画哪（不做 vx/vy 外推——KF 速度噪声会把
        // 静止目标放大成可见漂移）；新检测到达即整体更新位置
        this.detQ = core.pushDet(this.detQ, { fp, objs: msg.targets || [] });
    }

    start() {
        const tick = () => {
            this._tick();
            this._raf = requestAnimationFrame(tick);
        };
        this._raf = requestAnimationFrame(tick);
    }

    stop() {
        cancelAnimationFrame(this._raf);
        this._raf = 0;
    }

    // 视频侧"实时边缘"秒值：按模式提取（唯一分叉点，详设 §6.3 / 前端设计 §6.3）
    _videoEdgeSec() {
        const v = this.video;
        try {
            if (this.mode === 'mse') {
                // 缓冲前沿≈实时；★忌用 currentTime（播放头落后实时 300~800ms）
                if (v.buffered && v.buffered.length > 0) {
                    return v.buffered.end(v.buffered.length - 1);
                }
                return NaN;  // 缓冲未就绪，不锚
            }
            // webrtc：MediaStream 无播放缓冲，播放头即实时边缘
            return v.currentTime;
        } catch (_) {
            return NaN;
        }
    }

    _tick() {
        // 页面不可见暂停绘制（WS 照收，回页 detQ 补齐弹出最新）
        if (!this.videoReady || document.hidden) return;
        this._syncCanvas();
        if (this.anchorFp === null || this.detW === 0) { this._clear(); return; }

        // ③ 逐帧对齐：视频当前画面在"检测域"坐标 = currentTime×1e6 + 零点锚
        const vc = this.video.currentTime * 1e6 + this.anchorFp;
        const r = core.alignTick(this.detQ, vc);
        this.detQ = r.detQ;
        if (r.cur) this.lastObjs = r.cur.objs;

        // ④ 直接画最近一次检测的目标（无外推）；检测无目标 → 清空。
        // ROI 静态层与检测框同 fitRect 基准（归一化 × 画面矩形）——画幅/分屏
        // 变化时 fitRect 重算，区域自动跟随
        const fit = core.fitRect(this.detW, this.detH,
                                 this.canvas.width, this.canvas.height);
        const rois = core.drawRois(this.rois, fit, this.detW, this.detH);
        const items = core.drawData(this.lastObjs || [], fit,
                                    this.canvas.width, this.canvas.height,
                                    this.classes);
        this._draw(items, rois);
    }

    // canvas 像素尺寸 = 容器 CSS 尺寸 × dpr（cap 2x）；CSS 铺满格子，
    // video 用 object-fit: contain —— 实际渲染矩形与 fitRect 同源，框与画面天然对齐
    _syncCanvas() {
        const box = this.canvas.parentElement;
        if (!box) return;
        const rect = box.getBoundingClientRect();
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        const w = Math.max(1, Math.round(rect.width * dpr));
        const h = Math.max(1, Math.round(rect.height * dpr));
        if (this.canvas.width !== w || this.canvas.height !== h) {
            this.canvas.width = w;
            this.canvas.height = h;
        }
    }

    _clear() {
        const { width, height } = this.canvas;
        if (width > 0 && height > 0) this.ctx.clearRect(0, 0, width, height);
    }

    _draw(items, rois = []) {
        const ctx = this.ctx;
        this._clear();
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        // ROI 虚线区域（检测框下层；逐块颜色来自配置页颜色卡，空 = 默认主题色
        // + 极浅填充——多算法多区域可按色区分归属）
        if (rois.length) {
            ctx.setLineDash([6 * dpr, 4 * dpr]);
            ctx.lineWidth = Math.max(1, Math.round(1.5 * dpr));
            for (const r of rois) {
                ctx.strokeStyle = r.color || 'rgba(76,195,255,.85)';
                ctx.fillStyle = r.color
                    ? r.color + '14'                      // 8% 透明填充（hex 加 alpha）
                    : 'rgba(76,195,255,.08)';
                ctx.beginPath();
                ctx.moveTo(r.pts[0][0], r.pts[0][1]);
                for (let i = 1; i < r.pts.length; i++) ctx.lineTo(r.pts[i][0], r.pts[i][1]);
                ctx.closePath();
                ctx.fill();
                ctx.stroke();
            }
            ctx.setLineDash([]);  // 恢复实线（检测框）
        }
        for (const it of items) {
            ctx.strokeStyle = it.color;
            ctx.lineWidth = Math.max(1, Math.round(1.5 * dpr));
            ctx.strokeRect(it.x, it.y, it.w, it.h);
            // 标签：左上角实底条 + 类名 置信度（工控高对比）
            ctx.font = `${Math.round(11 * dpr)}px monospace`;
            const tw = ctx.measureText(it.label).width;
            const lh = Math.round(15 * dpr);
            ctx.fillStyle = it.color;
            ctx.fillRect(it.x, Math.max(0, it.y - lh), tw + 4 * dpr, lh);
            ctx.fillStyle = '#16181d';
            ctx.fillText(it.label, it.x + 2 * dpr, Math.max(lh - 3 * dpr, it.y - 3 * dpr));
        }
    }
}
