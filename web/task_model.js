// Form conversion is independent of the DOM and mirrors public task constraints.
export const taskStates = {
    draft: '草稿', pending: '等待确认', running: '运行中', disabled: '已停用',
    waiting_schedule: '等待时间窗口', suspended: '摄像头已停用',
    partially_suspended: '部分摄像头已停用', deleted: '已删除',
};
export function minuteText(minute) {
    return `${String(Math.floor(minute / 60)).padStart(2, '0')}:${String(minute % 60).padStart(2, '0')}`;
}
export function parseMinute(text, end = false) {
    if (!/^\d{2}:\d{2}$/.test(text)) throw new Error('时间请填写 HH:MM');
    const [h, m] = text.split(':').map(Number);
    if (m > 59 || h > 24 || (h === 24 && (!end || m !== 0))) throw new Error('时间范围应为 00:00–24:00，24:00 仅用于结束时间');
    return h * 60 + m;
}
export function parsePolygon(text) {
    const points = text.trim().split(';').map((point) => point.trim().split(',').map((n) => {
        if (!n.trim()) throw new Error('区域坐标不能为空');
        return Number(n);
    }));
    if (points.length < 3 || points.length > 128 || points.some((p) => p.length !== 2 || p.some((n) => !Number.isFinite(n) || n < 0 || n > 1)))
        throw new Error('区域需要 3–128 个点，坐标范围为 0–1');
    const area = points.reduce((s, p, i) => {
        const q = points[(i + 1) % points.length]; return s + p[0] * q[1] - q[0] * p[1];
    }, 0);
    if (Math.abs(area) < 1e-9) throw new Error('区域面积不能为零');
    return points;
}
export function scheduleValue({ timezone, weekdays, start, end }) {
    if (!/^UTC(?:[+-](?:0\d|1[0-3]):[0-5]\d|[+-]14:00)?$/.test(timezone))
        throw new Error('时区请填写 UTC 或固定偏移，例如 UTC+08:00');
    if (!Number.isInteger(weekdays) || weekdays < 1 || weekdays > 127) throw new Error('请选择至少一天');
    const start_minute = parseMinute(start), end_minute = parseMinute(end, true);
    if (start_minute === end_minute) throw new Error('开始与结束时间不能相同；全天请填写 00:00–24:00');
    return { timezone, weekdays, start_minute, end_minute };
}
export function editableTask(task) {
    return task.kind === 'v2' && !task.enabled && ['draft', 'disabled'].includes(task.status);
}

// A saved target can still be waiting, rejected, or superseded. Never infer runtime
// success from `accepted`, or from a missing `pending` field alone.
export function taskTransition(result) {
    if (!result?.accepted) return { state: 'rejected', tone: 'err', message: '任务请求未受理，请刷新状态后重试' };
    const operations = result.operations || [];
    if (operations.some((operation) => operation.apply_status === 'failed'))
        return { state: 'failed', tone: 'err', message: '任务目标已保存，但部分摄像头应用失败，请查看执行详情' };
    if (operations.some((operation) => operation.apply_status === 'superseded'))
        return { state: 'superseded', tone: 'warn', message: '本次配置已被后续修改替代，请载入最新任务' };
    if (result.pending || !result.applied || operations.some((operation) => operation.apply_status !== 'succeeded'))
        return { state: 'pending', tone: 'warn', message: '任务目标已保存，等待摄像头确认' };
    return { state: 'confirmed', tone: 'ok', message: result.status === 'waiting_schedule' ? '任务目标已确认，等待时间窗口' : '任务目标已确认' };
}

// Validate related form fields together so removing an algorithm cannot silently
// leave its rules pointing at another model. The server remains authoritative.
export function validateTaskDraft(body) {
    const byteLength = (text) => new TextEncoder().encode(text).length;
    if (!body.name || byteLength(body.name) > 128) throw new Error('任务名称不能为空，且最多 128 个 UTF-8 字节');
    if (!Array.isArray(body.camera_ids) || !body.camera_ids.length || body.camera_ids.length > 64 ||
        new Set(body.camera_ids).size !== body.camera_ids.length)
        throw new Error('请选择 1–64 台不同的摄像头');
    if (!body.algorithms.length || body.algorithms.length > 64) throw new Error('请添加 1–64 个算法');
    const models = new Set();
    for (const algorithm of body.algorithms) {
        if (!algorithm.algo_id || models.has(algorithm.algo_id)) throw new Error('算法不能为空或重复，同一模型只需添加一次');
        models.add(algorithm.algo_id);
        if (!Number.isInteger(algorithm.fps) || algorithm.fps < 1 || algorithm.fps > 25)
            throw new Error('分析 FPS 必须为 1–25 的整数');
        if (!Number.isFinite(algorithm.threshold) || algorithm.threshold < 0 || algorithm.threshold > 1)
            throw new Error('置信度阈值必须在 0–1 之间');
    }
    if (body.rules.length > 256) throw new Error('一个任务最多保存 256 条规则');
    const names = new Set();
    for (const rule of body.rules) {
        if (!rule.key || byteLength(rule.key) > 64 || names.has(rule.key)) throw new Error('规则名称不能为空或重复，且最多 64 个 UTF-8 字节');
        names.add(rule.key);
        if (!models.has(rule.algo_id)) throw new Error(`规则“${rule.key}”引用的算法未添加，请检查算法列表`);
        if (!['region_in', 'dwell'].includes(rule.capability_id)) throw new Error('当前只支持区域入侵和驻留规则');
        parsePolygon(rule.roi.map((point) => point.join(',')).join(';'));
        if (!Number.isInteger(rule.params.debounce_sec) || rule.params.debounce_sec < 0 || rule.params.debounce_sec > 4294967295)
            throw new Error('防抖秒数必须为有效的非负整数');
        if (rule.capability_id === 'dwell' && (!Number.isInteger(rule.params.dwell_sec) || rule.params.dwell_sec < 1 || rule.params.dwell_sec > 4294967295))
            throw new Error('驻留秒数必须为有效的正整数');
    }
    if (body.schedules.length > 64) throw new Error('一个任务最多保存 64 个时间窗口');
    return body;
}
