// overlay_core.js —— 检测框叠加纯函数核心（任务分解 T5.1，详设 §6.3）
//
// 可测试性纪律（任务分解 §1.1）：无 DOM、无 import，全部状态以参数传入、
// 返回新状态（可序列化）。overlay.js 是薄壳：DOM 事件 → 调这里 → 画 canvas。
//
// 对齐方案（详设 §6.3 + 前端设计 §6.3 双模式裁决）：
//   detections 与视频帧同源（同一子码流），但两条通道各自在"连接时刻"归零，
//   零点差恒定。锚定 = 用「实时边缘」对拍求零点差：
//     mse    → video.buffered.end(last)（缓冲前沿≈实时；★忌用 currentTime）
//     webrtc → video.currentTime（MediaStream 无播放缓冲，播放头即实时边缘）
//   锚定后逐帧对齐：弹出所有 frame_pts ≤ currentTime×1e6 + anchor 的检测取最新。
//   换网络只让"框跟着视频一起延迟"，相对对齐不变——免校准。

// detQ 容量 64 丢最旧（详设 §6.3，demo 原 30→64）
export const DETQ_CAP = 64;
// 陈旧阈值：vc - fp 超过 2s 的帧在对齐阶段被消费/丢弃（详设 §6.3）
export const STALE_US = 2e6;

// 框位置策略（板上裁决，原 vx/vy 外推移除）：
//   检测在哪就画哪，新检测到达即整体更新位置，无中间推进。
//   原因：ByteTrack KF 的速度过程噪声（kQv=25，为运动目标容忍加减速而设）
//   使静止目标速度估计不归零（±5~15px/s 游走），外推放大成可见漂移；
//   检测帧率 ≥ 算法 fps，逐帧跳变视觉可接受且零漂移。

// 工控深色风格高对比色板（前端设计 §1/§4.3；按 cls 取色）
export const PALETTE = [
    '#34c77b', '#4d9fff', '#f5a524', '#ff6f61', '#c792ea', '#ffd766',
];

export function colorOf(cls) {
    return PALETTE[Math.abs(cls | 0) % PALETTE.length];
}

// ---- ① 零点锚定（仅一次，锚后不再变） ----
// anchorFp: 现有锚（null=未锚）；fp_us: 最新 detections 的 frame_pts（us）；
// videoEdgeSec: 视频侧"实时边缘"秒值（壳层按模式提取：buffered.end 或 currentTime）。
// 返回 anchorFp（us）或 null（edge 无效不锚，等下一条）。
export function computeAnchor(anchorFp, mode, fpUs, videoEdgeSec) {
    if (anchorFp !== null && anchorFp !== undefined) return anchorFp;
    if (!Number.isFinite(fpUs) || !Number.isFinite(videoEdgeSec)) return null;
    if (fpUs < 0 || videoEdgeSec < 0) return null;
    return fpUs - videoEdgeSec * 1e6;
}

// ---- ② 入队（容量 64 丢最旧） ----
// item = { fp: us, objs: targets 数组 }；返回新队列（不修改入参）
export function pushDet(detQ, item) {
    const q = detQ.concat([item]);
    return q.length > DETQ_CAP ? q.slice(q.length - DETQ_CAP) : q;
}

// ---- ③ 逐帧对齐（详设 §6.3 tick A 段原样） ----
// 弹出所有 fp ≤ vc 的检测取最新；随后陈旧防御循环（队首比 vc 旧 2s+ 时丢弃，
// 详设原样保留——正常时序下第一步已消费陈旧帧，此循环兜底异常堆积）。
// 返回 { cur, detQ }；cur=null 表示本轮无新检测（沿用 lastObjs）。
export function alignTick(detQ, vcUs) {
    let cur = null;
    let i = 0;
    while (i < detQ.length && detQ[i].fp <= vcUs) {
        cur = detQ[i];
        ++i;
    }
    detQ = detQ.slice(i);
    while (detQ.length > 0 && vcUs - detQ[0].fp > STALE_US) {
        detQ = detQ.slice(1);
    }
    return { cur, detQ };
}

// ---- ④ fitRect：检测域 → canvas 等比 contain + 居中 ----
// 与 RGA letterbox 同构（scale = min(dst/src)，对称 padding）；往返误差 0。
export function fitRect(srcW, srcH, dstW, dstH) {
    if (!srcW || !srcH || !dstW || !dstH) return { scale: 1, ox: 0, oy: 0 };
    const scale = Math.min(dstW / srcW, dstH / srcH);
    return {
        scale,
        ox: (dstW - srcW * scale) / 2,
        oy: (dstH - srcH * scale) / 2,
    };
}

// ---- ⑤ 绘制指令生成（tick C 段的纯函数化；DOM 壳照抄执行） ----
// objs: 最近一次对齐命中的目标（检测域坐标）；fit: fitRect 输出；cw/ch: canvas 像素尺寸；
// classes: cls→类名映射（来自 /api/algorithms）。异常框（非有限值/零尺寸/完全
// 出界）剔除。返回 [{x,y,w,h,label,trackId,color}]。
export function drawData(objs, fit, cw, ch, classes = []) {
    const items = [];
    for (const o of objs || []) {
        const { x, y, w, h } = o;
        if (![x, y, w, h].every(Number.isFinite)) continue;
        if (w <= 0 || h <= 0) continue;
        const px = x * fit.scale + fit.ox;
        const py = y * fit.scale + fit.oy;
        const pw = w * fit.scale;
        const ph = h * fit.scale;
        if (px + pw < 0 || py + ph < 0 || px > cw || py > ch) continue;
        // 类名：映射命中用真名（person/bus…）；未命中回退 "cls{id}"——
        // COCO id 直接显示数字（如 0）会像 bug
        const name = classes[o.cls] !== undefined ? classes[o.cls] : `cls${o.cls}`;
        const conf = Number.isFinite(o.conf) ? Math.round(o.conf * 100) : null;
        items.push({
            x: px, y: py, w: pw, h: ph,
            label: conf !== null ? `${name} ${conf}%` : name,
            trackId: o.track_id,
            color: colorOf(o.cls),
        });
    }
    return items;
}

// ---- ⑥ ROI 多边形绘制指令（事件接线：宫格/单路显示已保存的检测区域）----
// rois: [{pts:[[x,y],...], color?}] 归一化顶点（0~1，一通道可多算法各一块，
// color = 配置页颜色卡选的 #rrggbb，缺省由 overlay 用默认主题色）；
// fit: fitRect 输出；srcW/srcH: 检测域分辨率（frame_w/frame_h）——
// 归一化先乘源帧尺寸换算成帧像素，再经 fitRect 映射（与检测框同一语义链：
// norm × src × scale + ox），画面矩形含 letterbox 居中，画幅变化自动跟随。
// 顶点数 <3 或含非有限值的整块剔除。返回 [{pts:[[px,py],...], color}]。
export function drawRois(rois, fit, srcW, srcH) {
    const items = [];
    if (!srcW || !srcH) return items;
    for (const roi of rois || []) {
        if (!roi || !Array.isArray(roi.pts) || roi.pts.length < 3) continue;
        let ok = true;
        const pts = roi.pts.map(([x, y]) => {
            const nx = Number(x) * srcW * fit.scale + fit.ox;
            const ny = Number(y) * srcH * fit.scale + fit.oy;
            if (!Number.isFinite(nx) || !Number.isFinite(ny)) ok = false;
            return [nx, ny];
        });
        if (!ok) continue;
        items.push({ pts, color: typeof roi.color === 'string' ? roi.color : '' });
    }
    return items;
}
