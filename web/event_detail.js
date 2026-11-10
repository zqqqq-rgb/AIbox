import { api } from './api.js';
import { h, fmtMs } from './ui.js';

// Fetch the persisted event by its local event ID, never by camera ID.
export function showEventDetail(id, { cameraName = {}, classesOf = {} } = {}) {
    const root = document.getElementById('modal-root');
    const previousFocus = document.activeElement;
    const mask = h('div', 'mask');
    const dialog = h('div', 'modal wide');
    dialog.setAttribute('role', 'dialog'); dialog.setAttribute('aria-modal', 'true');
    dialog.setAttribute('aria-label', `事件 #${id} 详情`);
    const title = h('div', 'm-title', `事件 #${id} 详情`);
    const body = h('div', 'm-body');
    const actions = h('div', 'm-btns');
    const closeButton = h('button', 'btn', '关闭');
    let alive = true;
    function close() {
        if (!alive) return;
        alive = false; mask.remove(); window.removeEventListener('keydown', escape);
        if (previousFocus?.isConnected) previousFocus.focus();
    }
    function escape(event) { if (event.key === 'Escape') close(); }
    window.addEventListener('keydown', escape);
    closeButton.onclick = close;
    mask.onclick = (event) => { if (event.target === mask) close(); };
    actions.append(closeButton); dialog.append(title, body, actions); mask.append(dialog); root.append(mask);
    closeButton.focus();
    async function load() {
        body.replaceChildren(h('p', 'dim', '正在查询已保存的事件…'));
        try {
            const detail = await api.getEvent(id, { silent: true });
            if (!alive) return;
            body.replaceChildren();
            const fields = h('dl', 'event-detail-fields');
            const items = [
                ['事件 ID', detail.id], ['发生时间', fmtMs(detail.ts)],
                ['摄像头', `${cameraName[detail.camera_id] || '摄像头'}（cam${detail.camera_id}）`],
                ['算法', detail.algo_id], ['事件类型', detail.type],
                ['任务 ID', detail.task_id || '历史事件未记录'], ['规则 ID', detail.rule_id || '历史事件未记录'],
                ['任务版本', detail.task_revision || '—'], ['规则版本', detail.rule_version || '—'],
                ['上报标记', detail.reported ? '已上报' : '未上报'],
            ];
            for (const [label, value] of items) fields.append(h('dt', 'dim', label), h('dd', 'mono', String(value ?? '—')));
            body.append(fields);
            const targets = Array.isArray(detail.targets) ? detail.targets : [];
            body.append(h('h3', '', `检测目标（${targets.length}）`));
            if (detail.targets_valid === false) body.append(h('p', 'warn-txt', '历史目标数据无法解析，事件基本信息仍可查看。'));
            else if (!targets.length) body.append(h('p', 'dim', '没有记录目标信息'));
            for (const target of targets) {
                const confidence = target.conf ?? target.score;
                body.append(h('p', 'mono small', `轨迹 ${target.track_id ?? '—'} · ${classesOf[target.cls] ?? `类别 ${target.cls ?? '—'}`} · 置信度 ${Number.isFinite(confidence) ? (confidence * 100).toFixed(1) + '%' : '—'}`));
            }
            if (detail.snapshot) {
                const image = h('img', 'event-detail-snapshot'); image.alt = '事件快照';
                image.src = '/recordings/snap/' + encodeURIComponent(String(detail.snapshot).split(/[\\/]/).pop());
                image.onerror = () => { if (alive) image.replaceWith(h('p', 'dim', '快照暂不可读取')); };
                body.append(image);
            } else body.append(h('p', 'dim', '该事件尚无快照'));
        } catch (failure) {
            if (!alive) return;
            const message = h('p', 't-err', `详情查询失败：${failure.message}`); message.setAttribute('role', 'alert');
            const retry = h('button', 'btn', '重试'); retry.onclick = load;
            body.replaceChildren(message, retry);
        }
    }
    load();
    return close;
}
