// ui.js —— 工控深色 UI 三件套（前端设计 §1 视觉规范 / §4.2 三态视图）
// toast / confirmModal / badge / tri；DOM 访问全部在函数内（node 导入安全）

const KIND_COLOR = { ok: 'var(--ok)', warn: 'var(--warn)', err: 'var(--off)', info: 'var(--acc)' };

// ---- toast：右下角叠放，5s 自动消失（事件弹窗卡片同用） ----
export function toast(msg, kind = 'info', ms = 3500) {
    const root = document.getElementById('toasts');
    if (!root) return;
    const el = document.createElement('div');
    el.className = `toast t-${kind}`;
    el.style.borderLeftColor = KIND_COLOR[kind] || KIND_COLOR.info;
    el.textContent = String(msg);
    root.appendChild(el);
    while (root.children.length > 6) root.firstChild.remove();
    setTimeout(() => el.remove(), ms);
    return el;
}

// ---- confirm modal：Promise<boolean>（删除/回滚级联确认） ----
export function confirmModal({ title = '确认', body = '', danger = false } = {}) {
    return new Promise((resolve) => {
        const root = document.getElementById('modal-root');
        if (!root) { resolve(false); return; }
        const mask = document.createElement('div');
        mask.className = 'mask';
        mask.innerHTML = `
            <div class="modal">
              <div class="m-title"></div>
              <div class="m-body"></div>
              <div class="m-btns">
                <button class="btn" data-a="no">取消</button>
                <button class="btn ${danger ? 'btn-danger' : 'btn-acc'}" data-a="yes">确认</button>
              </div>
            </div>`;
        mask.querySelector('.m-title').textContent = title;
        mask.querySelector('.m-body').textContent = body;
        const done = (v) => { mask.remove(); resolve(v); };
        mask.addEventListener('click', (e) => {
            if (e.target === mask) done(false);
            const a = e.target.getAttribute && e.target.getAttribute('data-a');
            if (a === 'yes') done(true);
            if (a === 'no') done(false);
        });
        root.appendChild(mask);
    });
}

// ---- 徽标：online 状态灯 ----
export function badge(online, text) {
    const el = document.createElement('span');
    el.className = `badge ${online ? 'b-ok' : 'b-off'}`;
    el.textContent = text !== undefined ? text : online ? '在线' : '离线';
    return el;
}

// ---- 三态视图：loading / empty / error / clear（§4.2） ----
// error 态带重试按钮；empty 态可带引导动作节点
export function tri(el, state, { onRetry, emptyText = '暂无数据', emptyAction = null } = {}) {
    if (!el) return;
    if (state === 'clear') { el.innerHTML = ''; return; }
    el.innerHTML = '';
    const box = document.createElement('div');
    box.className = 'tri';
    if (state === 'loading') {
        box.innerHTML = '<div class="spin"></div>';
    } else if (state === 'empty') {
        box.innerHTML = `<div class="dim">${emptyText}</div>`;
        if (emptyAction) box.appendChild(emptyAction);
    } else if (state === 'error') {
        const t = document.createElement('div');
        t.className = 't-err';
        t.textContent = '加载失败';
        const b = document.createElement('button');
        b.className = 'btn btn-acc';
        b.textContent = '重试';
        b.onclick = () => onRetry && onRetry();
        box.append(t, b);
    }
    el.appendChild(box);
}

// ---- 通用元素快捷 ----
export function h(tag, cls, text) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text !== undefined) el.textContent = text;
    return el;
}

export function fmtMs(ms) {
    if (!ms) return '--';
    const d = new Date(Number(ms));
    return Number.isFinite(d.getTime()) ? d.toLocaleString('sv') : '--';  // sv: YYYY-MM-DD HH:mm:ss
}

export function fmtUptime(s) {
    if (!s || s < 0) return '--';
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
    return `${h}h${m}m`;
}
