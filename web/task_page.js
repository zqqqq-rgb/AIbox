import { api } from './api.js';
import { h, toast, confirmModal, fmtMs } from './ui.js';
import { taskStates, minuteText, parsePolygon, scheduleValue, editableTask, taskTransition, validateTaskDraft } from './task_model.js';

const fullRegion = [[0, 0], [1, 0], [1, 1], [0, 1]];
function input(value = '', type = 'text') {
    const el = h('input'); el.type = type; el.value = value; return el;
}
function select(options, value) {
    const el = h('select');
    for (const [id, label] of options) { const o = h('option', '', label); o.value = id; el.append(o); }
    if (value !== undefined) el.value = value;
    return el;
}
function field(label, control) {
    const el = h(['INPUT', 'SELECT', 'TEXTAREA'].includes(control.tagName) ? 'label' : 'div', 'task-field');
    el.append(h('span', 'dim small', label), control); return el;
}
function button(text, action, cls = '') { const b = h('button', `btn ${cls}`, text); b.type = 'button'; b.onclick = action; return b; }
function polygon(initial, selectedCameras) {
    const wrap = h('div');
    const canvas = h('canvas', 'task-region'); canvas.width = 480; canvas.height = 270;
    canvas.setAttribute('aria-label', '检测区域，单击添加顶点；也可在坐标框输入');
    const camera = select([]), imageState = h('div', 'small dim');
    imageState.setAttribute('role', 'status');
    let image = null, imageLoaded = false, imageGeneration = 0, disposed = false;
    const text = input((initial || fullRegion).map((p) => p.join(',')).join(';'));
    text.setAttribute('aria-label', '区域顶点坐标');
    const draw = () => {
        const ctx = canvas.getContext('2d'); ctx.clearRect(0, 0, canvas.width, canvas.height);
        if (imageLoaded) ctx.drawImage(image, 0, 0, canvas.width, canvas.height);
        try {
            const points = text.value.trim().split(';').filter(Boolean).map((p) => p.split(',').map(Number));
            ctx.fillStyle = '#4cc3ff22'; ctx.strokeStyle = '#4cc3ff'; ctx.lineWidth = 2; ctx.beginPath();
            points.forEach(([x, y], i) => i ? ctx.lineTo(x * canvas.width, y * canvas.height) : ctx.moveTo(x * canvas.width, y * canvas.height));
            ctx.closePath(); ctx.fill(); ctx.stroke();
            for (const [x, y] of points) { ctx.beginPath(); ctx.arc(x * canvas.width, y * canvas.height, 4, 0, Math.PI * 2); ctx.fillStyle = '#4cc3ff'; ctx.fill(); }
        } catch (_) { /* Validation runs before saving. */ }
    };
    canvas.onclick = (event) => {
        if (canvas.closest('fieldset')?.disabled) return;
        const rect = canvas.getBoundingClientRect();
        if (!rect.width || !rect.height) return;
        const point = [Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)), Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height))].map((n) => Number(n.toFixed(3)));
        text.value += `${text.value.trim() ? ';' : ''}${point.join(',')}`; draw();
    };
    text.oninput = draw;
    const tools = h('div', 'row');
    tools.append(button('清空重画', () => { text.value = ''; draw(); }), button('撤销顶点', () => { text.value = text.value.split(';').slice(0, -1).join(';'); draw(); }), button('整幅画面', () => { text.value = fullRegion.map((p) => p.join(',')).join(';'); draw(); }));
    function releaseImage() {
        ++imageGeneration;
        if (image) { image.onload = null; image.onerror = null; image.src = ''; }
        image = null; imageLoaded = false;
    }
    function loadSnapshot() {
        if (disposed) return;
        releaseImage(); canvas.height = 270; draw();
        if (!camera.value) { imageState.textContent = '请选择关联摄像头。当前为空白区域示意，不是实时画面。'; return; }
        imageState.textContent = '正在获取摄像头快照…';
        const generation = imageGeneration, requested = new Image(); image = requested;
        requested.onload = () => {
            if (disposed || generation !== imageGeneration) return;
            if (!requested.naturalWidth || !requested.naturalHeight) { requested.onerror(); return; }
            imageLoaded = true;
            canvas.height = Math.max(1, Math.round(canvas.width * requested.naturalHeight / requested.naturalWidth));
            imageState.textContent = '已加载单帧快照，用于区域定位；不是实时视频。'; draw();
        };
        requested.onerror = () => {
            if (disposed || generation !== imageGeneration) return;
            imageLoaded = false;
            imageState.textContent = '快照不可用，当前为空白区域示意；可继续绘制或稍后刷新底图。'; draw();
        };
        requested.src = api.snapshotUrl(Number(camera.value));
    }
    function refreshCameras() {
        if (disposed) return;
        const previous = camera.value;
        camera.replaceChildren();
        for (const item of selectedCameras()) {
            const option = h('option', '', `${item.name} (#${item.id})`); option.value = item.id; camera.append(option);
        }
        if ([...camera.options].some((option) => option.value === previous)) camera.value = previous;
        if (camera.value !== previous || !image) {
            releaseImage(); canvas.height = 270; draw();
            imageState.textContent = camera.value
                ? '点击“加载 / 刷新底图”获取单帧快照。当前为空白区域示意。'
                : '请选择关联摄像头。当前为空白区域示意，不是实时画面。';
        }
    }
    camera.onchange = loadSnapshot;
    const imageTools = h('div', 'row');
    imageTools.append(field('底图摄像头', camera), button('加载 / 刷新底图', loadSnapshot));
    wrap.append(imageTools, imageState, h('div', 'small dim', '区域坐标按比例应用于所有关联摄像头，请分别核对各摄像头的安装视角。'), canvas, field('区域顶点（x,y；范围 0–1；以英文分号分隔，可直接输入）', text), tools);
    refreshCameras(); draw();
    return { el: wrap, read: () => parsePolygon(text.value), refreshCameras, dispose() { disposed = true; releaseImage(); } };
}

export async function pageTasks() {
    const wrap = h('div', 'page');
    const head = h('div', 'row-between'); head.append(h('h2', '', '算法任务'));
    const notice = h('div', 'small dim', '时间计划使用固定 UTC 偏移；多个窗口满足任意一个即可，跨午夜窗口归开始日。启用会预留峰值资源。');
    const error = h('div', 't-err'); error.setAttribute('role', 'alert');
    const layout = h('div', 'task-layout'), list = h('div', 'task-list'), editor = h('div', 'panel');
    layout.append(list, editor); wrap.append(head, notice, error, layout);
    document.getElementById('view').append(wrap);
    let alive = true, busy = false, loading = false, editing = null, rows = [], timer;
    let releaseRegions = [], editorGeneration = 0;
    let cameras = [], models = [], capabilities = [];
    const progress = h('div');
    const inspection = h('div');
    const capabilityPanel = h('details', 'panel task-capabilities');
    const capabilityRows = h('div', 'task-capability-grid');
    const capabilityStatus = h('p', 'small'); capabilityStatus.setAttribute('role', 'status');
    const capabilityRefresh = button('刷新能力状态', async () => {
        capabilityRefresh.disabled = true;
        capabilityStatus.textContent = '正在读取引擎能力…';
        try {
            const result = await api.listCapabilities({ silent: true });
            if (!alive) return;
            capabilities = result;
            renderCapabilities();
        } catch (e) {
            if (alive) { capabilityRows.replaceChildren(); capabilityStatus.textContent = `读取能力失败：${e.message}`; }
        } finally { capabilityRefresh.disabled = false; }
    });
    capabilityPanel.append(h('summary', '', '能力可用性'), capabilityRefresh, capabilityStatus, capabilityRows);
    wrap.insertBefore(capabilityPanel, layout);
    const statusText = { available: '可用', blocked: '受阻', unsupported: '不支持', waiting: '等待', inactive: '未运行', partial: '部分就绪' };
    function addFinding(parent, finding) {
        parent.append(h('p', finding.status === 'available' ? 'ok-txt' : 'small', finding.reason));
        if (finding.next_step) parent.append(h('p', 'small dim', finding.next_step));
    }
    function renderCapabilities() {
        capabilityRows.replaceChildren();
        capabilityStatus.textContent = capabilities.some((row) => row.simulation === true) ? '当前为模拟推理环境' : '';
        const names = { region_in: '区域入侵', dwell: '区域驻留', state_switch: '状态变化', line_cross: '越线', count: '计数', absence: '缺失', loitering: '徘徊', proximity: '接近', speed: '速度', compound: '组合规则', ref_geom: '参考几何' };
        for (const capability of capabilities) {
            const row = h('section', 'task-diagnostic-row');
            row.append(h('strong', '', `${names[capability.id] || capability.name} · ${statusText[capability.status] || capability.status}`));
            addFinding(row, capability);
            for (const model of capability.models || [])
                row.append(h('p', 'small dim', `${model.algo_id}${model.version ? ` v${model.version}` : ''} · ${statusText[model.status]}${model.status === 'available' ? '' : ` · ${model.reason}`}`));
            capabilityRows.append(row);
        }
    }
    async function runDiagnostics(task, control) {
        const generation = editorGeneration;
        control.disabled = true;
        inspection.replaceChildren(h('p', 'small dim', '正在核对任务配置…'));
        try {
            const result = await api.getTaskDiagnostics(task.id, { silent: true });
            if (!alive || generation !== editorGeneration) return;
            const report = h('section', 'task-diagnostic-report');
            report.append(h('h3', '', '任务诊断结果'));
            addFinding(report, result.summary);
            const engineState = { online: '引擎已连接', engine_offline: '引擎未连接', engine_syncing: '引擎同步中', engine_declaration_missing: '引擎未声明诊断能力', engine_query_failed: '引擎状态暂不可读取', executor_unavailable: '任务执行器未启动' };
            report.append(h('p', 'small dim', `${result.simulation === true ? '模拟推理 · ' : ''}${engineState[result.engine_state] || '引擎状态未知'} · 任务版本 ${result.task_revision} · 核对时间 ${fmtMs(result.observed_at)}`));
            for (const issue of result.model_issues || []) {
                const row = h('section', 'task-diagnostic-row');
                row.append(h('strong', '', `${issue.algo_id}${issue.rule_key ? ` · ${issue.rule_key}` : ''}`));
                addFinding(row, issue); report.append(row);
            }
            for (const camera of result.cameras || []) {
                const row = h('section', 'task-diagnostic-row');
                row.append(h('strong', '', `${camera.camera_name || '摄像头'} (#${camera.camera_id}) · ${statusText[camera.status]}`));
                addFinding(row, camera);
                row.append(h('p', 'small mono', `目标版本 ${camera.desired_revision} · 业务已确认 ${camera.applied_revision} · 引擎回报 ${camera.engine_revision ?? '无'}`));
                if (camera.operation?.last_error) row.append(h('p', 'small warn-txt', camera.operation.last_error));
                if (camera.operation?.next_retry_at > 0) row.append(h('p', 'small dim', `重试时间 ${fmtMs(camera.operation.next_retry_at)}`));
                report.append(row);
            }
            inspection.replaceChildren(report);
        } catch (e) {
            if (alive && generation === editorGeneration)
                inspection.replaceChildren(h('p', 't-err', `读取任务诊断失败：${e.message}`));
        } finally { control.disabled = false; }
    }
    const newButton = button('新建任务', () => open(null), 'btn-acc'); head.append(newButton);
    function diagnostics() {
        progress.replaceChildren();
        if (!editing?.id) return;
        const task = rows.find((t) => t.id === editing.id);
        if (!task) return;
        progress.append(h('h3', '', '当前执行状态'), h('div', 'krow', `${taskStates[task.status] || task.status} · 版本 ${task.revision} · ${task.applied ? '目标已确认' : '目标尚未确认'}`));
        if (task.revision !== editing.revision) progress.append(h('div', 'warn-txt', '任务版本已变化，请重新选择此任务以载入最新配置。'));
        for (const d of task.deployments || []) {
            const line = h('div', 'task-operation');
            line.append(h('span', 'small', `摄像头 ${d.camera_id}：${d.configuration_status} · 确认 ${d.applied_revision}/${d.desired_revision}`));
            line.append(button('查看执行详情', async () => {
                const generation = editorGeneration;
                try {
                    const operation = await api.getOperation(d.current_operation_id || d.operation_id);
                    if (!alive || generation !== editorGeneration) return;
                    const detail = h('div', 'small dim');
                    detail.textContent = `状态 ${operation.status} · 尝试 ${operation.attempts || 0} 次 · ${operation.last_error || '无错误'}`;
                    inspection.replaceChildren(detail);
                } catch (e) { if (alive && generation === editorGeneration) error.textContent = e.message; }
            })); progress.append(line);
        }
    }
    async function reload() {
        if (loading || !alive) return;
        loading = true;
        try {
            const tasks = await api.listTasks({ silent: true });
            if (!alive) return;
            rows = tasks.filter((t) => t.kind === 'v2'); list.replaceChildren();
            if (!rows.length) list.append(h('div', 'dim pad', '暂无 V2 任务，点击“新建任务”开始。'));
            for (const task of rows) {
                const b = button('', () => open(task)); b.className = 'task-card';
                b.append(h('strong', '', task.name), h('span', 'small dim', `${taskStates[task.status] || task.status} · ${task.camera_ids.length} 路摄像头 · v${task.revision}`));
                if (editing?.id === task.id) b.classList.add('selected'); list.append(b);
            }
            diagnostics();
        } catch (e) { if (alive) error.textContent = `任务列表刷新失败：${e.message}`; }
        finally { loading = false; }
    }
    async function perform(action) {
        if (busy) return;
        busy = true; error.textContent = '';
        const buttons = [...editor.querySelectorAll('button')].map((button) => [button, button.disabled]);
        buttons.forEach(([button]) => { button.disabled = true; });
        try { await action(); }
        catch (e) { if (alive) error.textContent = e.message; }
        finally { busy = false; if (alive) buttons.forEach(([button, disabled]) => { if (button.isConnected) button.disabled = disabled; }); }
    }
    function open(task) {
        if (!alive || busy) return;
        ++editorGeneration;
        releaseRegions.forEach((release) => release()); releaseRegions = [];
        editing = task ? structuredClone(task) : null; error.textContent = ''; editor.replaceChildren();
        inspection.replaceChildren();
        const readonly = task ? !editableTask(task) : false;
        editor.append(h('h3', '', task ? task.name : '新建草稿'));
        if (readonly) editor.append(h('div', 'warn-txt', '编辑前请先停用任务，并等待所有摄像头确认。'));
        const form = h('form');
        const fields = h('fieldset'); fields.disabled = readonly;
        const name = input(task?.name || ''); name.required = true; name.maxLength = 128;
        fields.append(field('任务名称', name));
        const cameraGroup = h('div', 'task-choices');
        const choices = cameras.map((cam) => {
            const box = input('', 'checkbox'); box.checked = (task?.camera_ids || []).includes(cam.id);
            box.onchange = () => rules.forEach((rule) => rule.refreshCameras());
            const label = h('label'); label.append(box, document.createTextNode(` ${cam.name} (#${cam.id})${cam.enabled ? '' : ' · 已停用'}`));
            cameraGroup.append(label); return { box, id: cam.id };
        });
        if (!cameras.length) cameraGroup.append(h('span', 'warn-txt', '请先在摄像头页面添加摄像头。'));
        fields.append(field('关联摄像头（可多选）', cameraGroup));
        const algoList = h('div'), ruleList = h('div'), scheduleList = h('div');
        let algorithms = [], rules = [], schedules = [], ruleNumber = 1;
        const ruleNames = new Set((task?.rules || []).map((r) => r.key));
        const modelOptions = models.map((m) => [m.algo_id, `${m.name || m.algo_id} (${m.algo_id})`]);
        for (const a of task?.algorithms || [])
            if (!modelOptions.some(([id]) => id === a.algo_id)) modelOptions.push([a.algo_id, `${a.algo_id}（当前不可用）`]);
        function addAlgorithm(a = {}) {
            const row = h('div', 'task-section'), model = select(modelOptions, a.algo_id), fps = input(a.fps ?? 3, 'number'), threshold = input(a.threshold ?? 0.45, 'number');
            fps.min = 1; fps.max = 25; threshold.min = 0; threshold.max = 1; threshold.step = 0.01;
            const enabled = input('', 'checkbox'); enabled.checked = a.enabled !== false;
            const item = { row, read: () => ({ algo_id: model.value, fps: Number(fps.value), threshold: Number(threshold.value), enabled: enabled.checked }) };
            row.append(field('算法', model), field('检测 FPS（1–25）', fps), field('置信度阈值（0–1）', threshold), field('参与执行', enabled), button('移除此算法', () => { algorithms = algorithms.filter((v) => v !== item); row.remove(); }));
            algorithms.push(item); algoList.append(row);
        }
        function addRule(r = {}) {
            let defaultName;
            do { defaultName = `规则 ${ruleNumber++}`; } while (ruleNames.has(defaultName));
            const row = h('div', 'task-section'), key = input(r.key || defaultName), model = select(modelOptions, r.algo_id);
            ruleNames.add(key.value);
            const capability = select(capabilities.filter((c) => c.draft_supported).map((c) => [c.id, c.id === 'dwell' ? '区域驻留' : '区域入侵']), r.capability_id);
            const event = input(r.params?.event_type || ''), dwell = input(r.params?.dwell_sec ?? 10, 'number'), debounce = input(r.params?.debounce_sec ?? 0, 'number');
            dwell.min = 1; debounce.min = 0;
            const region = polygon(r.roi, () => cameras.filter((cam) => choices.some((choice) => choice.id === cam.id && choice.box.checked))), enabled = input('', 'checkbox'); enabled.checked = r.enabled !== false;
            releaseRegions.push(region.dispose);
            const dwellField = field('驻留秒数', dwell); const change = () => { dwellField.hidden = capability.value !== 'dwell'; }; capability.onchange = change; change();
            const item = { row, refreshCameras: region.refreshCameras, read: () => {
                const params = { debounce_sec: Number(debounce.value) };
                if (event.value.trim()) params.event_type = event.value.trim();
                if (capability.value === 'dwell') params.dwell_sec = Number(dwell.value);
                return { key: key.value.trim(), algo_id: model.value, capability_id: capability.value, roi: region.read(), params, enabled: enabled.checked };
            } };
            row.append(field('规则名称（任务内唯一）', key), field('使用算法（须在上方添加）', model), field('规则类型', capability), field('事件名称（留空使用默认名称）', event), dwellField, field('防抖秒数（0 使用默认 30 秒）', debounce), region.el, field('启用此规则', enabled), button('移除此规则', () => { region.dispose(); rules = rules.filter((v) => v !== item); row.remove(); }));
            rules.push(item); ruleList.append(row);
        }
        function addSchedule(s = {}) {
            const row = h('div', 'task-section'), timezone = input(s.timezone || 'UTC+08:00'), start = input(minuteText(s.start_minute ?? 0)), end = input(minuteText(s.end_minute ?? 1440));
            const days = h('div', 'task-choices');
            const checkboxes = ['一', '二', '三', '四', '五', '六', '日'].map((day, i) => {
                const c = input('', 'checkbox'); c.checked = Boolean((s.weekdays ?? 127) & (1 << i));
                const label = h('label'); label.append(c, document.createTextNode(` 周${day}`)); days.append(label); return c;
            });
            const item = { row, read: () => scheduleValue({ timezone: timezone.value.trim(), weekdays: checkboxes.reduce((v, c, i) => v | (c.checked ? 1 << i : 0), 0), start: start.value, end: end.value }) };
            row.append(field('时区（固定偏移）', timezone), days, field('开始时间 HH:MM', start), field('结束时间 HH:MM（可填 24:00；早于开始表示跨午夜）', end), button('移除此窗口', () => { schedules = schedules.filter((v) => v !== item); row.remove(); }));
            schedules.push(item); scheduleList.append(row);
        }
        for (const a of task?.algorithms || [{}]) addAlgorithm(a);
        for (const r of task?.rules || [{}]) addRule(r);
        for (const s of task?.schedules || []) addSchedule(s);
        fields.append(h('h3', '', '算法'), algoList, button('添加算法', () => addAlgorithm()), h('h3', '', '规则与检测区域'), ruleList, button('添加规则', () => addRule()), h('h3', '', '时间计划（没有窗口时持续执行）'), scheduleList, button('添加时间窗口', () => addSchedule()));
        form.append(fields);
        const actions = h('div', 'row task-actions');
        if (!readonly) actions.append(button('保存草稿', () => perform(async () => {
            const body = { name: name.value.trim(), enabled: false, camera_ids: choices.filter((v) => v.box.checked).map((v) => v.id), algorithms: algorithms.map((v) => v.read()), rules: rules.map((v) => v.read()), schedules: schedules.map((v) => v.read()) };
            validateTaskDraft(body);
            if (task) body.revision = task.revision;
            const saved = await api.saveTask(task?.id, body);
            if (!alive) return;
            toast('草稿已保存', 'ok'); busy = false; open(saved); await reload();
        }), 'btn-acc'));
        if (task) {
            const diagnoseButton = button('运行诊断', () => runDiagnostics(task, diagnoseButton));
            actions.append(diagnoseButton);
            actions.append(button(task.enabled ? '停用任务' : '启用任务', () => perform(async () => {
                let result;
                try { result = await api.setTaskEnabled(task.id, task.revision, !task.enabled); }
                catch (failure) {
                    if (!failure.data?.accepted || failure.data.id !== task.id) throw failure;
                    result = failure.data;
                }
                if (!alive) return;
                const transition = taskTransition(result);
                toast(transition.message, transition.tone);
                busy = false; open(result); await reload();
                if (alive && ['failed', 'superseded', 'rejected'].includes(transition.state)) error.textContent = transition.message;
            }), 'btn-acc'));
            if (!readonly) actions.append(button('删除草稿', () => perform(async () => {
                if (!await confirmModal({ title: '删除任务', body: `确认删除“${task.name}”？历史事件会保留。`, danger: true }) || !alive) return;
                await api.deleteTask(task.id, task.revision);
                if (alive) { busy = false; open(null); await reload(); }
            }), 'btn-danger'));
            actions.append(button('预检摄像头计划', () => perform(async () => {
                const result = await api.previewTasks(task.camera_ids[0]);
                if (!alive) return;
                const detail = h('div', 'small');
                detail.textContent = `该摄像头全部已保存任务（含草稿）：${result.valid ? '规则可编译' : '规则有错误'} · 峰值预算 ${result.budget.ok ? '通过' : '未通过'} · 算力 ${Number(result.budget.compute_pct).toFixed(1)}% · ${(result.errors || []).join('；')} ${result.budget.message || ''}`;
                inspection.replaceChildren(detail);
            })));
        }
        form.onsubmit = (e) => e.preventDefault(); editor.append(actions, progress, inspection, form); diagnostics();
    }
    try {
        [cameras, models, capabilities] = await Promise.all([api.listCameras(), api.listAlgorithms(), api.listCapabilities()]);
        if (!alive) return { unmount() {} };
        models = models.filter((m) => !m.disabled);
        renderCapabilities();
        await reload(); if (alive) open(null);
        timer = setInterval(reload, 2000);
    } catch (e) { if (alive) error.textContent = e.message; }
    return { unmount() { alive = false; ++editorGeneration; clearInterval(timer); releaseRegions.forEach((release) => release()); releaseRegions = []; wrap.remove(); } };
}
