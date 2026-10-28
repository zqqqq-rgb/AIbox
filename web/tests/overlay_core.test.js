// overlay_core 纯函数单测（T5.1 DoD；板上裁决移除 vx/vy 外推——检测在哪画哪）
// 无 DOM：video 边缘值（buffered.end / currentTime）以数字参数直传
import { describe, it, expect } from 'vitest';
import {
    DETQ_CAP, STALE_US,
    computeAnchor, pushDet, alignTick,
    fitRect, drawData, drawRois, colorOf,
} from '../overlay_core.js';

// ---- ① pts 零点锚定 ----
describe('computeAnchor 锚定', () => {
    it('mse 模式：fp - buffered.end×1e6（缓冲前沿对拍）', () => {
        // 检测最新 fp=10s（检测域零点），视频缓冲前沿=4s（视频域零点）→ 零点差 6s
        expect(computeAnchor(null, 'mse', 10_000_000, 4.0)).toBe(6_000_000);
    });
    it('webrtc 模式：fp - currentTime×1e6（播放头即实时边缘）', () => {
        expect(computeAnchor(null, 'webrtc', 10_000_000, 2.5)).toBe(7_500_000);
    });
    it('锚后不再变（传入不同 edge 也返回原锚）', () => {
        expect(computeAnchor(6_000_000, 'mse', 99_000_000, 1.0)).toBe(6_000_000);
        expect(computeAnchor(6_000_000, 'webrtc', 50, 0)).toBe(6_000_000);
    });
    it('无效边缘（NaN/负值）不锚，等下一条', () => {
        expect(computeAnchor(null, 'mse', 10_000_000, NaN)).toBeNull();
        expect(computeAnchor(null, 'mse', 10_000_000, -1)).toBeNull();
        expect(computeAnchor(null, 'mse', -1, 1.0)).toBeNull();
    });
});

// ---- ⑤ 队列容量 64 丢最旧 ----
describe('pushDet 容量淘汰', () => {
    it('容量 64，满后丢最旧（详设 §6.3：30→64）', () => {
        let q = [];
        for (let i = 0; i < 70; ++i) q = pushDet(q, { fp: i * 1_000_000, objs: [] });
        expect(q.length).toBe(DETQ_CAP);
        expect(q[0].fp).toBe(6 * 1_000_000);   // 最旧 6 条被挤掉
        expect(q[63].fp).toBe(69 * 1_000_000);
    });
    it('未满时保序全留', () => {
        let q = [];
        q = pushDet(q, { fp: 1, objs: ['a'] });
        q = pushDet(q, { fp: 2, objs: ['b'] });
        expect(q.map((x) => x.fp)).toEqual([1, 2]);
    });
});

// ---- ② 逐帧对齐 ----
describe('alignTick 逐帧对齐', () => {
    it('弹出所有 fp ≤ vc 取最新（DoD ②）', () => {
        const q = [
            { fp: 1, objs: ['a'] }, { fp: 2, objs: ['b'] },
            { fp: 3, objs: ['c'] }, { fp: 4, objs: ['d'] },
        ];
        const r = alignTick(q, 3);
        expect(r.cur.objs).toEqual(['c']);           // 最新命中
        expect(r.detQ).toEqual([{ fp: 4, objs: ['d'] }]);  // 未来帧留队
    });
    it('vc 之前无帧 → cur=null（沿用 lastObjs）', () => {
        const q = [{ fp: 10, objs: ['x'] }];
        const r = alignTick(q, 5);
        expect(r.cur).toBeNull();
        expect(r.detQ).toEqual(q);
    });
    it('陈旧帧（vc-fp>2s）在对齐阶段被消费', () => {
        const q = [{ fp: 1, objs: ['stale'] }, { fp: 5, objs: ['fresh'] }];
        const r = alignTick(q, 9 * 1e6);
        expect(r.cur.objs).toEqual(['fresh']);
        expect(r.detQ.length).toBe(0);
    });
    it('未来帧挡队时保留（详设防御循环仅查队首，正常时序不触发）', () => {
        const q = [{ fp: 20 * 1e6, objs: [] }, { fp: 1, objs: ['old'] }];
        const r = alignTick(q, 5 * 1e6);
        expect(r.cur).toBeNull();
        expect(r.detQ).toEqual(q);  // 与详设 §6.3 第二个 while 原样一致：仅查队首
    });
});

// ---- fitRect / drawData ----
describe('fitRect（与 RGA letterbox 同构）', () => {
    it('同尺寸恒等', () => {
        expect(fitRect(640, 480, 640, 480)).toEqual({ scale: 1, ox: 0, oy: 0 });
    });
    it('4:3 → 16:9：scale=min，水平居中', () => {
        const f = fitRect(640, 480, 1280, 720);
        expect(f.scale).toBeCloseTo(1.5);              // min(2.0, 1.5)
        expect(f.ox).toBeCloseTo(160);                 // (1280-960)/2
        expect(f.oy).toBeCloseTo(0);
    });
    it('16:9 → 4:3：垂直居中', () => {
        const f = fitRect(1280, 720, 640, 480);
        expect(f.scale).toBeCloseTo(0.5);              // min(0.5, 0.667)
        expect(f.ox).toBeCloseTo(0);
        expect(f.oy).toBeCloseTo(60);                  // (480-360)/2
    });
    it('非法尺寸 → 恒等（防御，配合 frame_w=0 跳过绘制）', () => {
        expect(fitRect(0, 0, 100, 100)).toEqual({ scale: 1, ox: 0, oy: 0 });
    });
});

describe('drawData 绘制指令', () => {
    const fit = { scale: 1.5, ox: 160, oy: 0 };
    it('坐标经 fitRect 缩放 + 标签含类名与置信度', () => {
        const objs = [{ cls: 0, conf: 0.87, x: 100, y: 200, w: 64, h: 128, track_id: 42 }];
        const items = drawData(objs, fit, 1280, 720, ['person', 'car']);
        expect(items.length).toBe(1);
        expect(items[0].x).toBeCloseTo(100 * 1.5 + 160);
        expect(items[0].w).toBeCloseTo(64 * 1.5);
        expect(items[0].label).toContain('person');
        expect(items[0].label).toContain('87');
        expect(items[0].color).toBe(colorOf(0));
    });
    it('零尺寸/非有限值/完全出界剔除', () => {
        const objs = [
            { cls: 0, conf: 0.5, x: 0, y: 0, w: 0, h: 10, track_id: 1 },      // 零宽
            { cls: 0, conf: 0.5, x: NaN, y: 0, w: 1, h: 1, track_id: 2 },     // 非有限
            { cls: 1, conf: 0.5, x: -1e5, y: 0, w: 10, h: 10, track_id: 3 },  // 完全出界
            { cls: 1, conf: 0.5, x: 10, y: 10, w: 10, h: 10, track_id: 4 },   // 正常
        ];
        const items = drawData(objs, { scale: 1, ox: 0, oy: 0 }, 1280, 720, []);
        expect(items.length).toBe(1);
        expect(items[0].trackId).toBe(4);
    });
});

// ---- drawRois：归一化 ROI → 画面矩形内像素多边形（宫格虚线区域） ----
describe('drawRois ROI 绘制指令', () => {
    // 4:3 帧（640×480）contain 到 16:9 画布（1280×720）：scale=1.5 ox=160
    const SRC_W = 640, SRC_H = 480;
    const fit = fitRect(SRC_W, SRC_H, 1280, 720);
    it('归一化顶点经 src×fitRect 映射（与检测框同一语义链）', () => {
        // 归一化 (0,0) → 帧像素 (0,0) → 画布 (ox, oy) = (160, 0)
        // 归一化 (1,1) → 帧像素 (640,480) → 画布 (160+640×1.5, 480×1.5) = (1120, 720)
        const items = drawRois([{ pts: [[0, 0], [1, 0], [1, 1], [0, 1]] }], fit, SRC_W, SRC_H);
        expect(items.length).toBe(1);
        expect(items[0].pts[0][0]).toBeCloseTo(160);
        expect(items[0].pts[0][1]).toBeCloseTo(0);
        expect(items[0].pts[2][0]).toBeCloseTo(1120);
        expect(items[0].pts[2][1]).toBeCloseTo(720);
    });
    it('画幅变化 → 同一 ROI 顶点随 fitRect 重算（自适应要求）', () => {
        const roi = [{ pts: [[0.5, 0.5], [0.8, 0.5], [0.8, 0.9]] }];
        const f1 = fitRect(SRC_W, SRC_H, 1280, 720);
        const f2 = fitRect(SRC_W, SRC_H, 640, 480);  // 画布变小（恒等映射）
        const a = drawRois(roi, f1, SRC_W, SRC_H)[0];
        const b = drawRois(roi, f2, SRC_W, SRC_H)[0];
        expect(a.pts[0][0]).toBeCloseTo(160 + 0.5 * SRC_W * 1.5);
        expect(b.pts[0][0]).toBeCloseTo(0.5 * SRC_W);  // 320
        expect(b.pts[0][0]).not.toBeCloseTo(a.pts[0][0]);
    });
    it('顶点 <3 / 非数组 / 非有限值整块剔除；多块区域全保留', () => {
        const rois = [
            { pts: [[0, 0], [0.5, 0]] },                      // 2 点
            null,                                             // 非对象
            { pts: [[0, 0], [0.5, 0], [NaN, 0.5]] },          // 非有限
            { pts: [[0, 0], [0.5, 0], [0.5, 0.5]], color: '#ff8800' },   // 正常 A
            { pts: [[0.6, 0.6], [0.9, 0.6], [0.9, 0.9]] },    // 正常 B（无色）
        ];
        const items = drawRois(rois, fit, SRC_W, SRC_H);
        expect(items.length).toBe(2);
        expect(items[0].pts.length).toBe(3);
        expect(items[1].pts.length).toBe(3);
    });
    it('颜色透传：有色带色、无色空串（overlay 落默认主题色）', () => {
        const items = drawRois([
            { pts: [[0, 0], [1, 0], [1, 1]], color: '#ff8800' },
            { pts: [[0, 0], [1, 0], [1, 1]] },
            { pts: [[0, 0], [1, 0], [1, 1]], color: '' },
            { pts: [[0, 0], [1, 0], [1, 1]], color: 123 },    // 非字符串 → 空串
        ], fit, SRC_W, SRC_H);
        expect(items[0].color).toBe('#ff8800');
        expect(items[1].color).toBe('');
        expect(items[2].color).toBe('');
        expect(items[3].color).toBe('');
    });
    it('空/缺省输入返回空数组', () => {
        expect(drawRois([], fit, SRC_W, SRC_H)).toEqual([]);
        expect(drawRois(null, fit, SRC_W, SRC_H)).toEqual([]);
        expect(drawRois(undefined, fit, SRC_W, SRC_H)).toEqual([]);
        expect(drawRois([[[0, 0], [1, 0], [1, 1]]], fit, 0, 0)).toEqual([]);  // 帧尺寸未到
    });
});
