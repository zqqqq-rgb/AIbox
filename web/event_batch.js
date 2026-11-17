import { api } from './api.js';
import { createDispositionFields } from './event_detail.js';
import { h } from './ui.js';

const actions = [
    ['acknowledged', '批量确认'], ['processing', '批量开始处理'],
    ['resolved', '批量标记已处理'], ['ignored', '批量忽略'],
];

export function createEventBatch({ onApplied, onBusy, isCurrent }) {
    const panel = h('section', 'panel event-batch');
    panel.setAttribute('aria-label', '批量处置');
    const toolbar = h('div', 'row event-batch-toolbar');
    const count = h('strong', '');
    const action = h('select', 'sel');
    action.setAttribute('aria-label', '批量操作');
    action.append(new Option('选择批量操作', ''));
    for (const [value, label] of actions) action.append(new Option(label, value));
    const submit = h('button', 'btn btn-acc', '执行批量处置');
    const clearButton = h('button', 'btn', '清空选择');
    const fields = createDispositionFields();
    const hint = h('p', 'small dim');
    const message = h('p', 'small');
    message.setAttribute('aria-live', 'polite');
    toolbar.append(count, action, submit, clearButton);
    panel.append(toolbar, fields.element, hint, message);
    const selected = new Map();
    const rows = new WeakMap();
    let busy = false, needsRefresh = false;

    function sync() {
        const values = [...selected.values()];
        const common = actions.filter(([value]) => values.length && values.every((row) => row.allowed.includes(value)));
        for (const option of action.options) if (option.value)
            option.disabled = !common.some(([value]) => value === option.value);
        if (action.selectedOptions[0]?.disabled) action.value = '';
        const confirming = action.value === 'acknowledged';
        count.textContent = `已选 ${selected.size} 条`;
        action.disabled = busy || needsRefresh || common.length === 0;
        submit.disabled = action.disabled || !action.value || (confirming && !fields.hasIdentity());
        clearButton.disabled = busy || !selected.size;
        fields.element.hidden = !action.value;
        fields.requireIdentity(confirming);
        fields.setDisabled(busy || needsRefresh);
        hint.textContent = !values.length ? '勾选事件以进行批量处置。' : !common.length
            ? '所选事件没有共同可执行的操作。' : confirming ? '批量确认前请填写姓名和工号。' : '';
    }

    function clear() {
        for (const row of selected.values()) row.checkbox.checked = false;
        selected.clear();
        action.value = '';
        sync();
    }

    function removeRow(element) {
        const row = rows.get(element);
        if (!row || selected.get(row.id) !== row) return;
        row.checkbox.checked = false;
        selected.delete(row.id);
        sync();
    }

    async function save() {
        if (submit.disabled) return;
        let metadata;
        try { metadata = fields.read(); }
        catch (error) { message.textContent = error.message; return; }
        const payload = {
            disposition: action.value,
            events: [...selected.values()].map(({ id, revision }) => ({ id, revision })),
            ...metadata,
        };
        busy = true;
        onBusy(true);
        sync();
        message.textContent = '正在保存批量处置…';
        try {
            const result = await api.setEventDispositionBatch(payload, { silent: true });
            if (!isCurrent()) return;
            clear();
            fields.clearNote();
            onApplied(result.items);
            message.textContent = `已处置 ${result.items.length} 条事件。`;
        } catch (error) {
            if (!isCurrent()) return;
            if (error.code === 1002 || error.code === 1003) {
                needsRefresh = true;
                message.textContent = `事件 #${error.data?.failed_event_id ?? '—'} 的状态或版本已变化，整批未保存。请重新查询后再试。`;
            } else if (error.code === 1001) {
                message.textContent = error.data?.reason_code === 'operator_identity_required'
                    ? '请填写姓名和工号后再确认。' : `提交内容无效：${error.message}`;
            } else {
                needsRefresh = true;
                message.textContent = '无法确认保存结果，请重新查询。';
            }
        } finally {
            busy = false;
            if (isCurrent()) { onBusy(false); sync(); }
        }
    }

    action.onchange = sync;
    fields.element.addEventListener('input', sync);
    clearButton.onclick = clear;
    submit.onclick = save;
    sync();
    return {
        panel,
        isBusy: () => busy,
        addRow(element, event) {
            if (!Number.isSafeInteger(event.disposition_revision) || !Array.isArray(event.allowed_dispositions)) return;
            const checkbox = h('input', 'event-select');
            checkbox.type = 'checkbox';
            checkbox.setAttribute('aria-label', `选择事件 #${event.event_id}`);
            const row = { id: event.event_id, revision: event.disposition_revision, allowed: event.allowed_dispositions, checkbox };
            rows.set(element, row);
            checkbox.onchange = () => {
                if (busy || needsRefresh || (checkbox.checked && selected.size >= 50)) {
                    checkbox.checked = selected.get(row.id) === row;
                    if (!busy && !needsRefresh) message.textContent = '一次最多选择 50 条事件。';
                    return;
                }
                if (checkbox.checked) selected.set(row.id, row);
                else selected.delete(row.id);
                message.textContent = '';
                sync();
            };
            element.append(checkbox);
        },
        updateRow(element, event) {
            const row = rows.get(element);
            if (!row || !Number.isSafeInteger(event.disposition_revision) || !Array.isArray(event.allowed_dispositions)) return;
            if (event.disposition_revision !== row.revision) removeRow(element);
            row.revision = event.disposition_revision;
            row.allowed = event.allowed_dispositions;
        },
        removeRow,
        reset() { needsRefresh = false; message.textContent = ''; clear(); },
    };
}
