import { describe, it, expect } from 'vitest';
import { minuteText, parseMinute, parsePolygon, scheduleValue, editableTask, taskTransition, validateTaskDraft } from '../task_model.js';
describe('task form boundary values', () => {
    it('round-trips midnight and accepts 24:00 only as an end', () => {
        expect(minuteText(1440)).toBe('24:00');
        expect(parseMinute('24:00', true)).toBe(1440);
        for (const value of ['24:00', '25:00', '12:60', '1:00', '00:00x']) expect(() => parseMinute(value)).toThrow();
    });
    it('keeps overnight day ownership and fixed UTC offsets', () => {
        expect(scheduleValue({ timezone: 'UTC+08:00', weekdays: 1, start: '22:00', end: '02:00' })).toEqual({ timezone: 'UTC+08:00', weekdays: 1, start_minute: 1320, end_minute: 120 });
        for (const timezone of ['Asia/Shanghai', 'UTC+14:01', 'UTC+08:99']) expect(() => scheduleValue({ timezone, weekdays: 1, start: '00:00', end: '24:00' })).toThrow();
        expect(() => scheduleValue({ timezone: 'UTC', weekdays: 0, start: '00:00', end: '24:00' })).toThrow();
        expect(() => scheduleValue({ timezone: 'UTC', weekdays: 127, start: '12:00', end: '12:00' })).toThrow();
    });
    it('rejects malformed, degenerate and out-of-frame polygons', () => {
        expect(parsePolygon('0,0; 1,0; 1,1')).toEqual([[0,0],[1,0],[1,1]]);
        for (const value of ['', ',0;1,0;1,1', '0,0;1,1', '0,0;0.5,0.5;1,1', '0,0;2,0;1,1', '0,0;abc,0;1,1']) expect(() => parsePolygon(value)).toThrow();
    });
    it('requires confirmed disable before editing a V2 task', () => {
        expect(editableTask({ kind: 'v2', enabled: false, status: 'disabled' })).toBe(true);
        expect(editableTask({ kind: 'v2', enabled: false, status: 'pending' })).toBe(false);
        expect(editableTask({ kind: 'v2', enabled: true, status: 'waiting_schedule' })).toBe(false);
        expect(editableTask({ kind: 'legacy', enabled: false })).toBe(false);
        expect(editableTask({ kind: 'v2', enabled: false, status: 'unknown' })).toBe(false);
    });
});

describe('task operation acknowledgement', () => {
    it('does not infer application from acceptance or a missing pending field', () => {
        const result = { accepted: true, operations: [{ apply_status: 'running' }] };
        expect(taskTransition(result).state).toBe('pending');
        expect(taskTransition({ accepted: false, applied: true }).state).toBe('rejected');
        expect(taskTransition({ accepted: true, applied: true, operations: [{ apply_status: 'unknown' }] }).state).toBe('pending');
    });
    it('keeps terminal camera failures visible even when another operation is pending', () => {
        const operations = [{ camera_id: 1, apply_status: 'succeeded' }, { camera_id: 2, apply_status: 'failed' }];
        expect(taskTransition({ accepted: true, applied: false, pending: true, operations })).toMatchObject({ state: 'failed', tone: 'err' });
        operations[1].apply_status = 'superseded';
        expect(taskTransition({ accepted: true, pending: false, operations }).state).toBe('superseded');
    });
    it('explains confirmed scheduled tasks without claiming they are running', () => {
        const result = { accepted: true, applied: true, pending: false, status: 'waiting_schedule', operations: [{ apply_status: 'succeeded' }] };
        expect(taskTransition(result)).toMatchObject({ state: 'confirmed', tone: 'ok' });
        expect(taskTransition(result).message).toContain('等待时间窗口');
    });
});

function draft() {
    return {
        name: '入口', camera_ids: [1, 2], enabled: false,
        algorithms: [{ algo_id: 'person', threshold: 0.6, fps: 3, enabled: true }],
        rules: [
            { key: 'entry', algo_id: 'person', capability_id: 'region_in', roi: [[0, 0], [1, 0], [1, 1]], params: { debounce_sec: 0 }, enabled: true },
            { key: 'stay', algo_id: 'person', capability_id: 'dwell', roi: [[0, 0], [1, 0], [1, 1]], params: { debounce_sec: 30, dwell_sec: 12 }, enabled: true },
        ],
        schedules: [{ timezone: 'UTC+08:00', weekdays: 31, start_minute: 1320, end_minute: 120 }],
    };
}

describe('task form consistency', () => {
    it('preserves multiple independent rules and overnight schedules', () => {
        const value = draft(), saved = structuredClone(value);
        expect(validateTaskDraft(value)).toEqual(saved);
        expect(value).toEqual(saved);
    });
    it('rejects removing a model still referenced by rules and duplicate rule names', () => {
        const value = draft(); value.algorithms[0].algo_id = 'face';
        expect(() => validateTaskDraft(value)).toThrow('引用的算法未添加');
        const duplicate = draft(); duplicate.rules[1].key = duplicate.rules[0].key;
        expect(() => validateTaskDraft(duplicate)).toThrow('规则名称');
    });
    it('rejects fractional FPS, non-finite thresholds and fractional duration', () => {
        for (const change of [
            (value) => { value.algorithms[0].fps = 1.5; },
            (value) => { value.algorithms[0].threshold = NaN; },
            (value) => { value.rules[1].params.dwell_sec = 1.5; },
            (value) => { value.rules[0].params.debounce_sec = -1; },
        ]) {
            const value = draft(); change(value);
            expect(() => validateTaskDraft(value)).toThrow();
        }
    });
    it('uses backend UTF-8 byte limits for multilingual names', () => {
        const value = draft(); value.name = '入'.repeat(43);
        expect(() => validateTaskDraft(value)).toThrow('UTF-8');
        value.name = '入'.repeat(42);
        expect(validateTaskDraft(value).name).toBe(value.name);
    });
});
