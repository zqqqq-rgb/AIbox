// T5.2 模块可导入性回归（node 环境，无 DOM）：语法错误/坏 import 即挂
// 页面逻辑的正确性属 T5.2 手工清单 + M6 E2E，这里只防"改崩了还不知道"
import { describe, it, expect } from 'vitest';

describe('T5.2 模块导入', () => {
    it('ui.js / api.js / ws.js / player.js / overlay.js / app.js 全部可导入', async () => {
        const ui = await import('../ui.js');
        expect(typeof ui.toast).toBe('function');
        expect(typeof ui.confirmModal).toBe('function');
        expect(typeof ui.tri).toBe('function');

        const apiMod = await import('../api.js');
        expect(typeof apiMod.api.listCameras).toBe('function');
        expect(typeof apiMod.api.getStream).toBe('function');
        expect(typeof apiMod.setToken).toBe('function');

        const ws = await import('../ws.js');
        expect(typeof ws.onEvent).toBe('function');
        expect(typeof ws.acquireMeta).toBe('function');
        expect(typeof ws.startEvents).toBe('function');
        expect(typeof ws.onConnState).toBe('function');

        const player = await import('../player.js');
        expect(typeof player.CamPlayer).toBe('function');

        const ov = await import('../overlay.js');
        expect(typeof ov.MetaOverlay).toBe('function');

        // app.js 顶层在无 DOM 环境不 boot（typeof document 守卫）
        const app = await import('../app.js');
        expect(app).toBeTruthy();
    });

    it('api.js 错误码兜底文案（详设 §8.2）', async () => {
        const { ApiError } = await import('../api.js');
        expect(new ApiError(3001).message).toContain('NPU 算力预检超限');
        expect(new ApiError(3004, '').message).toContain('已自动回滚');
        expect(new ApiError(3004).message).toBe(new ApiError(3004).message);  // 稳定
        const e = new ApiError(3004);
        expect(e.code).toBe(3004);
    });
});
