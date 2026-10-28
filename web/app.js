// app.js —— 哈希路由 + 七页面装配（前端设计 §3 布局 / §5 页面设计）
// 路由表（详设 §6.1）：#/grid #/cameras #/algos #/config #/events #/recordings #/settings
// 页面契约：render(params) → { el, unmount }；unmount 必须
//   destroy 全部 player / stop overlay / release 全部 WS 租借 / off 事件（性能纪律 §8-1）
import { api, ApiError } from './api.js';
import { onEvent, onConnState, acquireMeta, startEvents } from './ws.js';
import { toast, confirmModal, badge, tri, h, fmtMs, fmtUptime } from './ui.js';
import { CamPlayer } from './player.js';
import { MetaOverlay } from './overlay.js';
import { colorOf } from './overlay_core.js';

const $view = () => document.getElementById('view');
let currentPage = null;   // { unmount }
let classesOf = {};       // cls → 类名（registry_ready 时刷新）

// ---- 路由 ----
const ROUTES = {
    '/grid': pageGrid, '/cameras': pageCameras, '/algos': pageAlgos,
    '/config': pageConfig, '/events': pageEvents,
    '/recordings': pageRecordings, '/settings': pageSettings,
};

function parseHash() {
    const raw = location.hash.replace(/^#/, '') || '/grid';
    const [path, qs] = raw.split('?');
    return { path, params: Object.fromEntries(new URLSearchParams(qs || '')) };
}

let routeSeq = 0;         // 路由序号：快速连点导航时丢弃过期页面的异步结果
async function route() {
    const seq = ++routeSeq;
    if (currentPage) {
        try { currentPage.unmount && currentPage.unmount(); } catch (e) { console.error(e); }
        currentPage = null;
    }
    // ★清空路由出口：页面函数都是 appendChild 追加，不清则切页变成"追加窗口"
    $view().innerHTML = '';
    const { path, params } = parseHash();
    const fn = ROUTES[path] || pageGrid;
    document.querySelectorAll('nav a').forEach((a) =>
        a.classList.toggle('act', a.getAttribute('href') === `#${path}`));
    try {
        const page = await fn(params);
        if (seq !== routeSeq) {
            // await 期间用户已切走：本页过期，立即回收（防旧页定时器/WS 租借泄漏）
            try { page.unmount && page.unmount(); } catch (e) { console.error(e); }
            return;
        }
        currentPage = page;
    } catch (e) {
        if (seq !== routeSeq) return;
        $view().innerHTML = '';
        $view().appendChild(h('div', 'tri t-err', `页面加载失败：${e.message}`));
    }
}
if (typeof window !== 'undefined') window.addEventListener('hashchange', route);

// ---- 顶栏（全局）：10s 轮询 systemInfo/resources + WS degrade/camera_status 实时 ----
let degradeLevel = 0;
function renderTopbar(info, res) {
    const set = (id, txt) => { const e = document.getElementById(id); if (e) e.textContent = txt; };
    const lamp = document.getElementById('tb-engine');
    if (lamp) lamp.className = `dot ${res && res.engine_online ? 'd-ok' : 'd-off'}`;
    set('tb-ready', info && info.ready ? '✓' : '…');
    set('tb-uptime', info ? fmtUptime(info.uptime_s) : '--');
    set('tb-cams', info ? `${info.cameras_online}/${info.cameras_total}` : '--');
    set('tb-degrade', degradeLevel > 0 ? `降级 L${degradeLevel}` : '无');
    const tb = document.getElementById('tb-degrade');
    if (tb) tb.className = degradeLevel > 0 ? 'warn-txt' : '';
    if (info && info.version) {
        const ver = document.getElementById('nav-ver');
        if (ver) ver.textContent = `v${info.version}`;
    }
}

async function pollTopbar() {
    try {
        const [info, res] = await Promise.all([
            api.systemInfo({ silent: true }),
            api.getResources({ silent: true }).catch(() => null),
        ]);
        renderTopbar(info, res);
        if (info && info.ws_port) startEvents(info.ws_port);
    } catch (_) { /* 静默：顶栏轮询失败不打扰 */ }
}

function bootTopbar() {
    pollTopbar();
    setInterval(pollTopbar, 10000);
    onEvent('degrade', (j) => {
        degradeLevel = j.level || 0;
        if (degradeLevel > 0) toast(`系统降级 L${degradeLevel}：${j.reason || ''}`, 'warn');
        pollTopbar();
    });
    onEvent('camera_status', () => pollTopbar());
    // 全局事件 toast 卡片（点击跳宫格对应路；events 页列表另做实时头插）
    onEvent('event', (j) => {
        const el = toast(`${fmtMs(j.ts)} ${j.type} @cam${j.id} ${j.algo}`, 'warn', 5000);
        if (el) {
            el.style.cursor = 'pointer';
            el.onclick = () => { location.hash = `#/grid?cam=${j.id}`; };
        }
    });
    // WS 断线横幅
    const banner = document.getElementById('ws-banner');
    onConnState((s) => {
        if (banner) banner.style.display = s === 'connected' ? 'none' : 'block';
    });
}

// ---- 公共：registry classes 映射（cls → 显示名，检测框标签用）----
// classes 元素两种形态：{id,name} 对象（manifest 配了 class_names）或裸数字
// （未配置）——映射键 = COCO id，值 = 名称；无名时回退 "cls{id}"（不裸显数字）
async function loadClasses() {
    try {
        const algos = await api.listAlgorithms({ silent: true });
        const m = {};
        for (const a of algos) {
            (a.classes || []).forEach((c) => {
                const id = typeof c === 'object' ? c.id : c;
                const name = typeof c === 'object' && c.name ? c.name : null;
                if (name) m[id] = name;
            });
        }
        // 原地更新（保持对象引用稳定）：MetaOverlay 构造时按引用捕获 classesOf，
        // 若整体重赋值，已创建的 overlay 永远持旧映射（板上实证：检测框恒显 cls0）
        for (const k of Object.keys(classesOf)) delete classesOf[k];
        Object.assign(classesOf, m);
    } catch (_) {}
}
onEvent('registry_ready', () => loadClasses());

// ============================================================ #/grid
// 每通道已保存 ROI 列表（listCameras 回显 algos[]；一通道多算法各一块）：
// 仅收"画了区域（≥3 顶点）且显示开关开"的算法，元素 {pts, color}——
// color 为配置页颜色卡选色（空 = overlay 默认主题色）；
// roi_display 缺省按显示兼容老数据
function roisOf(cam) {
    return (cam.algos || [])
        .filter((a) => Array.isArray(a.roi) && a.roi.length >= 3 &&
                       a.roi_display !== false)
        .map((a) => ({ pts: a.roi, color: a.roi_color || '' }));
}

function makeTile(cam, rois = [], big = false) {
    const tile = h('div', `tile ${big ? 'tile-big' : ''}`);
    const box = h('div', 'box');
    const video = document.createElement('video');
    video.muted = true; video.playsInline = true; video.autoplay = true;
    video.disablePictureInPicture = true;  // 去画中画按钮（监控画面不允许弹出小窗）
    const canvas = document.createElement('canvas');
    canvas.className = 'ov';
    const tip = h('div', 'tile-tip', '连接中...');
    box.append(video, canvas, tip);
    const bar = h('div', 'tile-bar');
    const bdg = badge(!!cam.online);
    const name = h('span', 'tile-name', cam.name);  // 宫格只显名称（camN 是 DB id，用户不需要看）
    const clock = h('span', 'tile-clock dim');
    bar.append(bdg, name, clock);
    tile.append(box, bar);

    let player = null, overlay = null, release = null;
    function setState(t, cls) { tip.textContent = t; tip.className = `tile-tip ${cls || ''}`; }

    if (cam.online) {
        if (typeof ZLMRTCClient === 'undefined') {
            setState('播放内核未加载', 't-err');
        } else {
            player = new CamPlayer(video, cam.id, {
                quality: 'sub', audio: big,   // 宫格静音，单路放大出声
                onState: (s) => {
                    if (s === 'playing') { setState(''); overlay.onVideoReady(); }
                    else if (s === 'connecting') setState('连接中...');
                    else if (s === 'retry') setState('重连中...', 't-warn');
                    else if (s === 'failed') setState('连接失败（等待重试）', 't-err');
                },
            });
            overlay = new MetaOverlay(video, canvas, { mode: 'webrtc', classes: classesOf });
            overlay.setRois(rois);  // 已保存检测区域（虚线，随画幅自适应）
            overlay.start();
            player.start();
            release = acquireMeta(cam.id, {
                onMsg: (j) => overlay.onMeta(j),
                onState: (s) => { if (s === 'connected') overlay.reset(); },  // 重连→重锚
            });
        }
    } else {
        setState('离线', 'dim');
    }
    tile.onclick = () => { location.hash = `#/grid?cam=${cam.id}`; };
    return {
        el: tile,
        clockTick() { clock.textContent = new Date().toLocaleTimeString('sv'); },
        unmount() {
            tile.onclick = null;
            if (release) release();
            if (overlay) overlay.stop();
            if (player) player.destroy();
        },
    };
}

async function pageGrid(params) {
    const wrap = h('div', 'page');
    const tools = h('div', 'grid-tools');
    const view = h('div', 'grid-wrap');
    wrap.append(tools, view);
    $view().appendChild(wrap);
    tri(view, 'loading');

    const cams = await api.listCameras();
    const focusId = Number(params.cam) || 0;
    // 宫格规模取 hash query（#/grid?n=4），缺省 9；单路放大恒 1
    const N = focusId ? 1 : ([1, 4, 9].includes(Number(params.n)) ? Number(params.n) : 9);
    let shown = cams;
    if (focusId) shown = cams.filter((c) => c.id === focusId);
    shown = shown.slice(0, N);

    if (!focusId) {
        for (const n of [1, 4, 9]) {
            const b = h('button', `btn ${N === n ? 'btn-acc' : ''}`, `${n}路`);
            b.onclick = () => { location.hash = `#/grid?n=${n}`; };
            tools.appendChild(b);
        }
        tools.appendChild(h('span', 'dim', '（单击格子放大；子码流+检测框）'));
    } else {
        const back = h('button', 'btn', '← 返回宫格');
        back.onclick = () => { location.hash = '#/grid'; };
        tools.appendChild(back);
    }

    view.innerHTML = '';
    view.className = `grid-wrap g${N}`;
    if (shown.length === 0) {
        tri(view, 'empty', { emptyText: '无在线摄像头，先去添加', emptyAction: goAddCamera() });
        return { unmount() {} };
    }
    const tiles = shown.map((c) => makeTile(c, roisOf(c), !!focusId));
    tiles.forEach((t) => view.appendChild(t.el));
    const clockTimer = setInterval(() => tiles.forEach((t) => t.clockTick()), 1000);

    // 单路放大：信息面板（算法摘要 + 解码指标）
    let extraTimer = 0;
    if (focusId) {
        const panel = h('div', 'focus-panel');
        view.appendChild(panel);
        const renderPanel = async () => {
            const cam = cams.find((c) => c.id === focusId);
            if (!cam) return;
            panel.innerHTML = `<b>${cam.name}</b>`;
            const algos = (cam.algos || []).map((a) =>
                `${a.algo_id} thr=${a.threshold} fps=${a.fps}`).join(' · ');
            panel.appendChild(h('div', 'dim', algos || '未配置算法'));
            try {
                const res = await api.getResources({ silent: true });
                const dec = res && res.cams && res.cams[String(focusId)] &&
                    res.cams[String(focusId)].dec;
                if (dec) panel.appendChild(h('div', 'mono dim', `dec in=${dec.in} out=${dec.out} drop=${dec.drop || 0}`));
            } catch (_) {}
        };
        renderPanel();
        extraTimer = setInterval(renderPanel, 10000);
    }

    return {
        unmount() {
            clearInterval(clockTimer);
            if (extraTimer) clearInterval(extraTimer);
            tiles.forEach((t) => t.unmount());
        },
    };
}

function goAddCamera() {
    const b = h('button', 'btn btn-acc', '＋ 添加摄像头');
    b.onclick = () => { location.hash = '#/cameras'; };
    return b;
}

// ============================================================ #/cameras
// 卡片网格 + WS-Discovery 扫描添加（卡片"添加"→ 账密弹窗 → ONVIF 取流 → 入库）
async function pageCameras() {
    const wrap = h('div', 'page');
    const head = h('div', 'row-between');
    head.appendChild(h('h2', '', '摄像头管理'));
    const headBtns = h('span', 'btns');
    const scanBtn = h('button', 'btn', '扫描网络');
    const addBtn = h('button', 'btn btn-acc', '＋ 手动添加');
    headBtns.append(scanBtn, addBtn);
    head.appendChild(headBtns);

    const cards = h('div', 'cam-cards');  // 已添加摄像头
    const scanArea = h('div', '');        // 扫描结果区（点击扫描后出现）
    wrap.append(head, cards, scanArea);
    $view().appendChild(wrap);

    let addedIps = new Set();   // 已添加设备 IP 集合（扫描结果"已添加"标记）
    let lastDevices = [];       // 最近一次扫描结果（添加成功后重渲染状态）

    // rtsp://[user:pass@]1.2.3.4:554/... → 1.2.3.4
    function ipOf(rtsp) {
        const m = /\/\/(?:[^/@]+@)?([\w.]+)/.exec(rtsp || '');
        return m ? m[1] : '';
    }

    const offHandlers = [
        onEvent('camera_status', (j) => {
            const bdg = cards.querySelector(`[data-cam="${j.id}"] .badge`);
            if (bdg) { bdg.className = `badge ${j.online ? 'b-ok' : 'b-off'}`; bdg.textContent = j.online ? '在线' : '离线'; }
        }),
    ];

    function camCard(c) {
        const card = h('div', `cam-card ${c.online ? '' : 'cc-off'}`);
        card.dataset.cam = String(c.id);
        const top = h('div', 'cc-top');
        top.append(badge(!!c.online), h('span', 'mono dim', `cam${c.id}`), h('b', 'cc-name', c.name));
        card.appendChild(top);
        card.appendChild(h('div', 'cc-url dim mono ellipsis', c.rtsp_main));
        const foot = h('div', 'cc-foot');
        foot.appendChild(h('span', 'dim small', `算法 ${(c.algos || []).length} 项`));
        const btns = h('span', 'btns');
        const cfgBtn = h('button', 'btn', '配置');
        cfgBtn.onclick = () => { location.hash = `#/config?cam=${c.id}`; };
        const editBtn = h('button', 'btn', '编辑');
        editBtn.onclick = () => cameraForm(c);
        const delBtn = h('button', 'btn btn-danger', '删除');
        delBtn.onclick = async () => {
            if (!(await confirmModal({
                title: `删除 cam${c.id} ${c.name}？`,
                body: '将级联：ZLM 拉流代理 ×2 + engine 通道 + 配置记录（逻辑删）',
                danger: true,
            }))) return;
            try {
                await api.deleteCamera(c.id);
                toast('已删除', 'ok');
                renderDevices();  // 该设备从"已添加"集合移除后刷新扫描卡片
                reload();
            } catch (_) {}
        };
        btns.append(cfgBtn, editBtn, delBtn);
        foot.appendChild(btns);
        card.appendChild(foot);
        return card;
    }

    async function reload() {
        tri(cards, 'loading');
        let cams;
        try {
            cams = await api.listCameras();
        } catch (_) {
            tri(cards, 'error', { onRetry: reload });
            return;
        }
        addedIps = new Set(cams.map((c) => ipOf(c.rtsp_sub) || ipOf(c.rtsp_main)));
        cards.innerHTML = '';
        if (!cams.length) {
            tri(cards, 'empty', { emptyText: '还没有摄像头：点右上「扫描网络」发现设备，或手动添加' });
            return;
        }
        for (const c of cams) cards.appendChild(camCard(c));
    }

    // ---- 扫描结果 ----
    function devCard(d) {
        const added = addedIps.has(d.ip);
        const isRtsp = d.kind === 'rtsp';
        const kindTxt = isRtsp
            ? `RTSP 设备${d.hik ? '（疑似海康）' : '（未开 ONVIF）'}`
            : 'ONVIF 设备';
        const card = h('div', `cam-card dev-card ${added ? 'cc-off' : ''}`);
        const top = h('div', 'cc-top');
        top.append(h('span', 'dot ' + (added ? 'd-ok' : 'd-acc')), h('b', 'mono', d.ip));
        card.appendChild(top);
        card.appendChild(h('div', 'cc-url dim mono ellipsis', d.xaddr));
        const foot = h('div', 'cc-foot');
        foot.appendChild(h('span', 'dim small', added ? '已在通道列表' : kindTxt));
        const btns = h('span', 'btns');
        const add = h('button', 'btn btn-acc', added ? '已添加' : '添加');
        if (added) add.disabled = true;
        else add.onclick = () => (isRtsp ? addFlowRtsp(d) : addFlow(d));
        btns.appendChild(add);
        foot.appendChild(btns);
        card.appendChild(foot);
        return card;
    }

    function renderDevices() {
        if (!scanArea.firstChild || !lastDevices.length) return;
        const grid = scanArea.querySelector('.cam-cards');
        if (!grid) return;
        grid.innerHTML = '';
        for (const d of lastDevices) grid.appendChild(devCard(d));
    }

    async function scan() {
        scanArea.innerHTML = '';
        const sec = h('div', 'panel');
        sec.appendChild(h('h3', '', '网络扫描结果'));
        const grid = h('div', 'cam-cards');
        sec.appendChild(grid);
        scanArea.appendChild(sec);
        tri(grid, 'loading');
        scanBtn.disabled = true;
        try {
            const data = await api.scanCameras();
            lastDevices = data.devices || [];
            grid.innerHTML = '';
            if (!lastDevices.length) {
                tri(grid, 'empty', { emptyText: '未发现设备（需与盒子同网段，且相机开启 ONVIF）' });
            } else {
                for (const d of lastDevices) grid.appendChild(devCard(d));
            }
        } catch (_) {
            tri(grid, 'error', { onRetry: scan });
        } finally {
            scanBtn.disabled = false;
        }
    }
    scanBtn.onclick = scan;
    addBtn.onclick = () => cameraForm(null);

    // ---- 添加流程：账密弹窗 → ONVIF 取流 → addCamera ----
    function addFlow(d) {
        const root = document.getElementById('modal-root');
        const mask = h('div', 'mask');
        const tail = d.ip.split('.').pop();
        mask.innerHTML = `
          <div class="modal"><div class="m-title">添加 ${d.ip}</div>
            <div class="m-body">
              <label>名称 <input id="d-name" type="text" value="摄像头-${tail}"></label>
              <label>用户名 <input id="d-user" type="text" placeholder="onvif 用户名"></label>
              <label>密码 <input id="d-pass" type="password" placeholder="onvif 密码"></label>
              <div class="dim small">通过 ONVIF 自动获取主/子码流地址并入库，随后自动创建拉流代理</div>
            </div>
            <div class="m-btns"><button class="btn" id="d-cancel">取消</button>
              <button class="btn btn-acc" id="d-ok">添加</button></div>
          </div>`;
        mask.querySelector('#d-cancel').onclick = () => mask.remove();
        mask.addEventListener('click', (e) => { if (e.target === mask) mask.remove(); });
        mask.querySelector('#d-ok').onclick = async () => {
            const user = mask.querySelector('#d-user').value.trim();
            const pass = mask.querySelector('#d-pass').value;
            const name = mask.querySelector('#d-name').value.trim();
            if (!user || !name) { toast('用户名与名称不能为空', 'warn'); return; }
            const okBtn = mask.querySelector('#d-ok');
            okBtn.disabled = true;
            okBtn.textContent = '取流中…';
            try {
                const uris = await api.getOnvifUris({ ip: d.ip, xaddr: d.xaddr, username: user, password: pass });
                // 失败时不 throw：优先提示后端 msg，同时把手动表单拉出来兜底
                try {
                    const data = await api.addCamera({ name, rtsp_main: uris.rtsp_main, rtsp_sub: uris.rtsp_sub, enabled: true });
                    mask.remove();
                    toast(`已添加（id=${data.id}）`, 'ok');
                    if (data.note) toast(data.note, 'warn', 8000);
                    addedIps.add(d.ip);
                    renderDevices();
                    reload();
                    return;
                } catch (_) {
                    mask.remove();
                    cameraForm({ name, rtsp_main: uris.rtsp_main, rtsp_sub: uris.rtsp_sub, manualPrefill: true });
                }
            } catch (e) {
                okBtn.disabled = false;
                okBtn.textContent = '添加';
                // 2002 账密错误 / 2001 取流失败：toast 已由 api.js 弹出，留在弹窗可改密码重试
                if (e && e.code === 2002) mask.querySelector('#d-pass').select();
            }
        };
        root.appendChild(mask);
    }

    // ---- RTSP 设备添加：路径预填（海康标准路径），账密注入后直接入库 ----
    function addFlowRtsp(d) {
        const root = document.getElementById('modal-root');
        const mask = h('div', 'mask');
        const tail = d.ip.split('.').pop();
        const pre = d.hik ? { main: '/Streaming/Channels/101', sub: '/Streaming/Channels/102' }
                          : { main: '', sub: '' };
        mask.innerHTML = `
          <div class="modal"><div class="m-title">添加 ${d.ip}（RTSP）</div>
            <div class="m-body">
              <label>名称 <input id="r-name" type="text" value="摄像头-${tail}"></label>
              <label>用户名 <input id="r-user" type="text" placeholder="rtsp 用户名"></label>
              <label>密码 <input id="r-pass" type="password" placeholder="rtsp 密码"></label>
              <label>主码流路径 <input id="r-main" type="text" value="${pre.main}"
                placeholder="海康：/Streaming/Channels/101"></label>
              <label>子码流路径 <input id="r-sub" type="text" value="${pre.sub}"
                placeholder="海康：/Streaming/Channels/102"></label>
              <div class="dim small">${d.hik ? '已按海康标准路径预填' : '大华路径：/cam/realmonitor?channel=1&subtype=0/1'}
                ；账密自动注入 rtsp://user:pass@${d.ip}:554/…</div>
            </div>
            <div class="m-btns"><button class="btn" id="r-cancel">取消</button>
              <button class="btn btn-acc" id="r-ok">添加</button></div>
          </div>`;
        mask.querySelector('#r-cancel').onclick = () => mask.remove();
        mask.addEventListener('click', (e) => { if (e.target === mask) mask.remove(); });
        mask.querySelector('#r-ok').onclick = async () => {
            const name = mask.querySelector('#r-name').value.trim();
            const user = mask.querySelector('#r-user').value.trim();
            const pass = mask.querySelector('#r-pass').value;
            const main = mask.querySelector('#r-main').value.trim();
            const sub = mask.querySelector('#r-sub').value.trim();
            if (!name || !user || !main || !sub) { toast('字段不能为空', 'warn'); return; }
            // 相对路径 → 完整 URI（账密 percent-encode 注入；大华路径含 &/= 合法保留）
            const base = `rtsp://${encodeURIComponent(user)}:${encodeURIComponent(pass)}@${d.ip}:554`;
            const uri = (p) => (p.startsWith('rtsp://') ? p : base + (p.startsWith('/') ? p : '/' + p));
            const okBtn = mask.querySelector('#r-ok');
            okBtn.disabled = true;
            okBtn.textContent = '添加中…';
            try {
                const data = await api.addCamera({ name, rtsp_main: uri(main), rtsp_sub: uri(sub), enabled: true });
                mask.remove();
                toast(`已添加（id=${data.id}）`, 'ok');
                if (data.note) toast(data.note, 'warn', 8000);
                addedIps.add(d.ip);
                renderDevices();
                reload();
            } catch (_) {
                // 账密/路径错 → ZLM 建代理失败 4002 toast 已弹，留在弹窗可改
                okBtn.disabled = false;
                okBtn.textContent = '添加';
            }
        };
        root.appendChild(mask);
    }

    function cameraForm(cam) {
        const root = document.getElementById('modal-root');
        const mask = h('div', 'mask');
        const isEdit = !!cam && !cam.manualPrefill;
        mask.innerHTML = `
          <div class="modal"><div class="m-title">${isEdit ? '编辑' : '添加'}摄像头</div>
            <div class="m-body">
              <label>名称 <input id="f-name" type="text"></label>
              <label>主码流 RTSP <input id="f-main" type="text" placeholder="rtsp://user:pass@ip:554/main"></label>
              <label>子码流 RTSP <input id="f-sub" type="text" placeholder="rtsp://user:pass@ip:554/sub"></label>
              <label class="chk"><input id="f-en" type="checkbox" checked> 启用</label>
              <div class="dim small">添加后自动创建 ZLM 拉流代理并下发 engine</div>
            </div>
            <div class="m-btns"><button class="btn" id="f-cancel">取消</button>
              <button class="btn btn-acc" id="f-ok">${isEdit ? '保存' : '添加'}</button></div>
          </div>`;
        if (cam) {
            mask.querySelector('#f-name').value = cam.name || '';
            mask.querySelector('#f-main').value = cam.rtsp_main || '';
            mask.querySelector('#f-sub').value = cam.rtsp_sub || '';
            // enabled 不在 GET /api/cameras 回包（只有 online），编辑默认勾选
            mask.querySelector('#f-en').checked = true;
        }
        mask.querySelector('#f-cancel').onclick = () => mask.remove();
        mask.addEventListener('click', (e) => { if (e.target === mask) mask.remove(); });
        mask.querySelector('#f-ok').onclick = async () => {
            const body = {
                name: mask.querySelector('#f-name').value.trim(),
                rtsp_main: mask.querySelector('#f-main').value.trim(),
                rtsp_sub: mask.querySelector('#f-sub').value.trim(),
                enabled: mask.querySelector('#f-en').checked,
            };
            if (!body.name || !body.rtsp_main || !body.rtsp_sub) { toast('字段不能为空', 'warn'); return; }
            try {
                const data = isEdit ? await api.updateCamera(cam.id, body) : await api.addCamera(body);
                mask.remove();
                toast(isEdit ? '已保存' : `已添加（id=${data.id}）`, 'ok');
                // engine add 失败但代理已建：后端 data.note 黄色警示（前端设计 §5.2）
                if (data && data.note) toast(data.note, 'warn', 8000);
                reload();
            } catch (_) {}
        };
        root.appendChild(mask);
    }

    await reload();
    return { unmount() { offHandlers.forEach((f) => f()); } };
}

// ============================================================ #/config

// ---- ROI 多边形编辑器（截图底图 + canvas 顶点拾取，归一化坐标保存）----
// 交互：点击加顶点 → 右键/「闭合」完成（≥3 点）→ 确定；撤销/清除可重画。
// 坐标基于截图自然分辨率内部换算、对外归一化 0~1（与 engine 判定一致，
// 与主/子码流实际分辨率解耦）
function openRoiEditor(camId, d, onSave) {
    const root = document.getElementById('modal-root');
    const mask = h('div', 'mask');
    mask.innerHTML = `
      <div class="modal wide"><div class="m-title">绘制检测区域：${d.algo_id}</div>
        <div class="roi-hint">点击画面添加顶点（≥3 个），右键或「闭合」完成。
          判定以目标脚部（检测框底边中点）进入区域为准。</div>
        <div class="roi-stage"><img id="roi-img" alt="通道截图加载中…">
          <canvas id="roi-cv"></canvas></div>
        <div class="m-btns" style="justify-content:space-between;">
          <div style="display:flex;gap:8px;">
            <button class="btn" id="roi-undo">撤销上一点</button>
            <button class="btn" id="roi-clear">清除重画</button>
            <button class="btn" id="roi-refresh">刷新截图</button>
            <button class="btn" id="roi-close" style="display:none">闭合</button>
          </div>
          <div style="display:flex;gap:8px;">
            <button class="btn" id="roi-cancel">取消</button>
            <button class="btn btn-acc" id="roi-ok">确定</button>
          </div>
        </div>
      </div>`;
    const img = mask.querySelector('#roi-img');
    const cv = mask.querySelector('#roi-cv');
    const closeBtn = mask.querySelector('#roi-close');
    const ctx = cv.getContext('2d');
    let pts = [];      // 画布内部坐标（px）
    let closed = false;
    let mouse = null;  // 橡皮筋预览端点

    function draw() {
        ctx.clearRect(0, 0, cv.width, cv.height);
        if (!pts.length) return;
        ctx.lineWidth = 2;
        ctx.strokeStyle = '#4cc3ff';
        ctx.fillStyle = 'rgba(76,195,255,.15)';
        ctx.beginPath();
        ctx.moveTo(pts[0][0], pts[0][1]);
        for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
        if (closed) {
            ctx.closePath();
            ctx.fill();
        } else if (mouse) {
            ctx.lineTo(mouse[0], mouse[1]);  // 橡皮筋
        }
        ctx.stroke();
        for (const [x, y] of pts) {
            ctx.fillStyle = '#ffffff';
            ctx.fillRect(x - 3, y - 3, 6, 6);
        }
    }
    function reset(pts0, closed0) {
        pts = pts0 || [];
        closed = !!closed0;
        closeBtn.style.display = closed ? 'none' : '';
        draw();
    }
    function toCv(e) {  // 显示坐标 → 画布内部坐标（CSS 缩放还原）
        const r = cv.getBoundingClientRect();
        return [(e.clientX - r.left) * (cv.width / r.width),
                (e.clientY - r.top) * (cv.height / r.height)];
    }

    img.onload = () => {
        cv.width = img.naturalWidth;
        cv.height = img.naturalHeight;
        // 已存 ROI 回显（归一化 → 画布坐标），直接可清除重画
        reset(d.roi && d.roi.length >= 3
                  ? d.roi.map(([x, y]) => [x * cv.width, y * cv.height]) : null,
              !!(d.roi && d.roi.length >= 3));
    };
    img.onerror = () => {
        img.alt = '';
        mask.querySelector('.roi-stage').innerHTML =
            '<div style="padding:40px;line-height:1.5;color:#ffb347;font-size:13px;">' +
            '截图失败（通道可能离线）。可稍后用「刷新截图」重试。</div>';
    };
    img.src = api.snapshotUrl(camId);

    cv.addEventListener('click', (e) => {
        if (closed) return;
        pts.push(toCv(e));
        if (pts.length >= 3) closeBtn.style.display = '';
        draw();
    });
    cv.addEventListener('mousemove', (e) => {
        if (closed) return;
        mouse = toCv(e);
        draw();
    });
    cv.addEventListener('mouseleave', () => { mouse = null; draw(); });
    cv.addEventListener('contextmenu', (e) => {
        e.preventDefault();
        if (!closed && pts.length >= 3) reset(pts, true);
    });
    closeBtn.onclick = () => { if (pts.length >= 3) reset(pts, true); };
    mask.querySelector('#roi-undo').onclick = () => {
        pts.pop();
        if (pts.length < 3) closed = false;
        closeBtn.style.display = closed ? 'none' : '';
        draw();
    };
    mask.querySelector('#roi-clear').onclick = () => reset(null, false);
    mask.querySelector('#roi-refresh').onclick = () => { img.src = api.snapshotUrl(camId); };
    mask.querySelector('#roi-cancel').onclick = () => mask.remove();
    mask.addEventListener('click', (e) => { if (e.target === mask) mask.remove(); });
    mask.querySelector('#roi-ok').onclick = () => {
        if (!closed || pts.length < 3) { toast('至少 3 个顶点并闭合后再确定', 'warn'); return; }
        const norm = pts.map(([x, y]) => [
            Number((Math.max(0, Math.min(1, x / cv.width))).toFixed(4)),
            Number((Math.max(0, Math.min(1, y / cv.height))).toFixed(4)),
        ]);
        mask.remove();
        onSave(norm);
    };
    root.appendChild(mask);
}

async function pageConfig(params) {
    const wrap = h('div', 'page');
    wrap.appendChild(h('h2', '', '通道算法配置'));
    const bar = h('div', 'row');
    const sel = document.createElement('select');
    sel.className = 'sel';
    const est = h('span', 'mono dim', '');
    bar.append(h('span', '', '摄像头：'), sel, est);
    const form = h('div', 'list');
    const saveBar = h('div', 'row-between');
    const saveBtn = h('button', 'btn btn-acc', '保存并热生效');
    saveBar.appendChild(saveBtn);
    wrap.append(bar, form, saveBar);
    $view().appendChild(wrap);

    const cams = await api.listCameras();
    const algos = await api.listAlgorithms();
    for (const c of cams) {
        const o = document.createElement('option');
        o.value = String(c.id);
        o.textContent = `cam${c.id} ${c.name}`;
        sel.appendChild(o);
    }
    const cur = cams.find((c) => c.id === Number(params.cam)) || cams[0];
    if (cur) sel.value = String(cur.id);

    // 本地编辑态：{algo_id, enabled, threshold, fps, roi, rule_type, dwell_sec,
    //              roi_display, roi_color}
    // roi = 归一化顶点数组 | null（null = 未画区域 → 仅检测不报警）
    // roi_display = 宫格是否显示该算法区域虚线（缺省 true）
    // roi_color = 宫格区域颜色 #rrggbb（'' = overlay 默认主题色）
    let draft = [];
    function buildForm(cam) {
        form.innerHTML = '';
        est.textContent = '';
        draft = algos.filter((a) => !a.disabled).map((a) => {
            const curCfg = (cam.algos || []).find((x) => x.algo_id === a.algo_id);
            const rule = curCfg && curCfg.rule ? curCfg.rule : null;
            return {
                algo_id: a.algo_id, name: a.name, trigger: a.trigger,
                enabled: !!curCfg,
                threshold: curCfg ? curCfg.threshold : (a.default_threshold || 0.45),
                fps: curCfg ? curCfg.fps : 3,
                roi: curCfg && Array.isArray(curCfg.roi) && curCfg.roi.length >= 3
                         ? curCfg.roi : null,
                rule_type: rule && rule.rule_type === 'dwell' ? 'dwell' : 'region_in',
                dwell_sec: rule && rule.dwell_sec ? rule.dwell_sec : 5,
                roi_display: curCfg ? curCfg.roi_display !== false : true,
                roi_color: (curCfg && curCfg.roi_color) || '#4cc3ff',
            };
        });
        for (const d of draft) {
            const row = h('div', 'lrow cfg-row');
            const cb = document.createElement('input');
            cb.type = 'checkbox'; cb.checked = d.enabled;
            cb.onchange = () => { d.enabled = cb.checked; sync(); };
            const thr = document.createElement('input');
            thr.type = 'number'; thr.step = '0.05'; thr.min = '0'; thr.max = '1';
            thr.value = d.threshold; thr.disabled = !d.enabled;
            thr.onchange = () => { d.threshold = Number(thr.value); };
            const fps = document.createElement('input');
            fps.type = 'number'; fps.min = '1'; fps.max = '25';
            fps.value = d.fps; fps.disabled = !d.enabled;
            fps.onchange = () => { d.fps = Number(fps.value); };
            // 规则类型（事件接线 V1：进入区域 / 区域内停留）
            const ruleSel = document.createElement('select');
            ruleSel.className = 'sel rule-ctl';
            ruleSel.innerHTML =
                '<option value="region_in">进入报警</option>' +
                '<option value="dwell">停留报警</option>';
            ruleSel.value = d.rule_type; ruleSel.disabled = !d.enabled;
            ruleSel.onchange = () => { d.rule_type = ruleSel.value; sync(); };
            const dwell = document.createElement('input');
            dwell.type = 'number'; dwell.min = '1'; dwell.max = '3600';
            dwell.title = '停留秒数'; dwell.value = d.dwell_sec;
            dwell.className = 'dwell-ctl'; dwell.disabled = !d.enabled;
            dwell.onchange = () => { d.dwell_sec = Math.max(1, Number(dwell.value) || 5); };
            const roiBtn = h('button', 'btn roi-ctl', d.roi ? '已画区域' : '画区域');
            if (d.roi) roiBtn.classList.add('btn-acc');
            roiBtn.disabled = !d.enabled;
            roiBtn.onclick = () => openRoiEditor(sel.value, d, (norm) => {
                d.roi = norm;
                roiBtn.textContent = '已画区域';
                roiBtn.classList.add('btn-acc');
            });
            // 宫格显示开关 + 颜色卡：区域视觉归属（勾选=宫格画该算法区域虚线）
            // showChk 带 .roi-ctl（sync 统一 disable）；外层 label 不带避免选择器落空
            const showChk = document.createElement('input');
            showChk.type = 'checkbox'; showChk.checked = d.roi_display;
            showChk.className = 'roi-ctl';
            showChk.title = '宫格显示该算法的检测区域';
            showChk.onchange = () => { d.roi_display = showChk.checked; };
            const colorInp = document.createElement('input');
            colorInp.type = 'color'; colorInp.value = d.roi_color;
            colorInp.className = 'roi-color-ctl';
            colorInp.title = '宫格区域颜色';
            colorInp.disabled = !d.enabled;
            colorInp.onchange = () => { d.roi_color = colorInp.value; };
            const showLbl = document.createElement('label');
            showLbl.className = 'chk small';
            showLbl.title = '宫格显示该算法的检测区域';
            showLbl.append(showChk, document.createTextNode('显示区域'));
            row.append(cb,
                h('span', 'mono', d.algo_id),
                h('span', 'dim', `${d.name}（${d.trigger === 'always' ? '常驻' : d.trigger}）`),
                h('span', '', '阈值'), thr, h('span', '', '帧率'), fps,
                ruleSel, dwell, roiBtn, showLbl, colorInp);
            form.appendChild(row);
        }
        if (!draft.length) tri(form, 'empty', { emptyText: '算法库为空（检查 packages 与 registry）' });
        sync();
    }
    function sync() {
        form.querySelectorAll('.cfg-row').forEach((row, i) => {
            const d = draft[i];
            if (!d) return;
            row.querySelectorAll('input[type=number], .rule-ctl, .roi-ctl, .roi-color-ctl')
                .forEach((el) => { el.disabled = !d.enabled; });
            row.querySelector('.dwell-ctl').style.display =
                d.rule_type === 'dwell' ? '' : 'none';
        });
    }
    function currentCam() { return cams.find((c) => String(c.id) === sel.value); }

    sel.onchange = () => buildForm(currentCam() || {});
    if (cur) buildForm(cur);

    saveBtn.onclick = async () => {
        const cam = currentCam();
        if (!cam) return;
        // ROI 已画才随 payload 发 roi+rule（set_camera_algos 全量替换：
        // 未携带 = 清除该算法的 ROI 与规则，语义即"不画区域不报警"）。
        // 显示开关与颜色独立于 roi 携带（画了区域才有视觉意义，但全量
        // 替换语义下统一携带，回显不丢用户设置）
        const payload = draft.filter((d) => d.enabled).map((d) => {
            const item = {
                algo_id: d.algo_id, threshold: d.threshold, fps: d.fps,
                enabled: 1,
                roi_display: d.roi_display ? 1 : 0,
                roi_color: d.roi_color || '',
            };
            if (d.roi && d.roi.length >= 3) {
                item.roi = d.roi;
                item.rule = { rule_type: d.rule_type, dwell_sec: d.dwell_sec };
            }
            return item;
        });
        try {
            // 预检拒绝（3001/3002）/通道忙（1003）：api 已 toast，表单保留可调参重试
            const data = await api.updateAlgos(cam.id, payload);
            const e = data && data.npu_est;
            est.textContent = e ? `NPU 预估：算力 ${Math.round(e.compute_pct)}% · 内存 ${Math.round(e.mem_pct)}%` : '';
            est.className = `mono ${e && (e.compute_pct > 80 || e.mem_pct > 80) ? 'warn-txt' : 'dim'}`;
            toast('已热生效（通道 RECONFIG）', 'ok');
        } catch (_) {}
    };

    return { unmount() {} };
}

// ============================================================ #/algos
async function pageAlgos() {
    const wrap = h('div', 'page');
    const head = h('div', 'row-between');
    head.appendChild(h('h2', '', '算法库'));
    const fresh = h('span', 'ok-txt', '');
    head.appendChild(fresh);
    const table = h('div', 'list');
    wrap.append(head, table);
    $view().appendChild(wrap);

    const off = onEvent('registry_ready', async () => {
        fresh.textContent = '算法库已更新';
        setTimeout(() => { fresh.textContent = ''; }, 5000);
        await reload();
    });

    async function reload() {
        tri(table, 'loading');
        const algos = await api.listAlgorithms();
        table.innerHTML = '';
        if (!algos.length) { tri(table, 'empty', { emptyText: '无已注册算法（检查 --packages 与 manifest）' }); return; }
        for (const a of algos) {
            const row = h('div', `lrow ${a.disabled ? 'lrow-off' : ''}`);
            row.append(
                h('span', 'mono', a.algo_id),
                h('span', '', a.name || '--'),
                h('span', 'dim mono', a.version || '--'),
                h('span', 'dim mono ellipsis', a.model_ref || '--'),
                h('span', 'mono', a.disabled ? '--' : `${a.npu_cost_ms.toFixed(1)}ms`),
                // npu_mem_kb=0 = 未估算（坏包/模型文件缺失）→ '--'
                h('span', 'mono',
                  a.disabled || !a.npu_mem_kb ? '--' : `${(a.npu_mem_kb / 1024).toFixed(0)}MB`),
                h('span', 'dim', a.trigger || '--'));
            if (a.disabled) row.appendChild(h('span', 'badge b-off', '已禁用'));
            else {
                const rb = h('button', 'btn', '⟳ 回滚');
                rb.onclick = async () => {
                    if (!(await confirmModal({
                        title: `回滚 ${a.algo_id}？`,
                        body: '回滚到上一版本包（engine reload：新 ctx 自检，失败自动回滚）',
                        danger: true,
                    }))) return;
                    try {
                        await api.rollbackAlgo(a.algo_id);
                        toast('reload 已下发', 'ok');
                    } catch (e) {
                        // 3004 = 自检失败已回滚：动作已执行，橙提示（前端设计 §5.4）
                        if (e instanceof ApiError && e.code === 3004) toast(e.message, 'warn');
                    }
                };
                row.appendChild(rb);
            }
            // classes 元素两种形态：数字（裸 COCO id，manifest 未配 class_names）
            // 或 {id,name} 对象（有名映射）——显示名优先，裸 id 加 "cls" 前缀
            // （直接显示 0 会像 bug；有映射时显示 person/bus 等真名）
            const cls = (a.classes || []).map((c, i) => {
                const id = typeof c === 'object' ? c.id : c;
                const name = typeof c === 'object' && c.name ? c.name : null;
                const label = name !== null ? name : `cls${id}`;
                return `<span class="chip" style="border-color:${colorOf(i)}">${label}</span>`;
            }).join('');
            const sub = h('div', 'lrow-sub');
            sub.innerHTML = cls || '<span class="dim">无类别表</span>';
            const cell = h('div', 'lcol');
            cell.append(row, sub);
            table.appendChild(cell);
        }
    }
    await reload();
    return { unmount() { off(); } };
}

// ============================================================ #/events
// 单列表布局：历史检索为主体；WS 实时事件按当前筛选条件头插进同一列表
// （原"实时流 | 历史检索"左右双栏已并入——实时区与历史区是同一份数据流）
async function pageEvents() {
    const wrap = h('div', 'page');
    wrap.appendChild(h('h2', '', '事件中心'));
    const histCol = h('div', 'col');
    const f = h('div', 'filters');
    const selCam = document.createElement('select'); selCam.className = 'sel';
    selCam.appendChild(new Option('全部摄像头', '0'));
    const inpAlgo = h('input', 'inp'); inpAlgo.placeholder = 'algo_id';
    const inpFrom = h('input', 'inp'); inpFrom.type = 'datetime-local';
    const inpTo = h('input', 'inp'); inpTo.type = 'datetime-local';
    const qBtn = h('button', 'btn btn-acc', '查询');
    // 实时接收开关：勾选 = WS 事件头插进列表；默认勾选（实时头插是页面主用途）
    // 复选框包进 label（label.chk flex 居中）——文字与框严格垂直对齐
    const liveChk = document.createElement('input');
    liveChk.type = 'checkbox'; liveChk.checked = true;
    const liveLbl = document.createElement('label');
    liveLbl.className = 'chk small';
    liveLbl.appendChild(liveChk);
    liveLbl.appendChild(document.createTextNode('实时'));
    f.append(selCam, inpAlgo, inpFrom, inpTo, qBtn, liveLbl);
    const histList = h('div', 'list');
    histCol.append(f, histList);
    wrap.appendChild(histCol);
    $view().appendChild(wrap);

    // 摄像头下拉 + id→名称映射（事件行名称列；离线摄像头事件也能显示名称）
    const camName = {};
    try {
        for (const c of await api.listCameras({ silent: true })) {
            selCam.appendChild(new Option(`cam${c.id} ${c.name}`, String(c.id)));
            camName[c.id] = c.name || `cam${c.id}`;
        }
    } catch (_) {}

    // 事件行（历史查询与 WS 头插共用；入参 = WS 契约形态）。
    // 两列布局：上行 = 时间/摄像头/算法名/规则类型/快照徽标；
    //          下行 = 触发 class 芯片（targets 的 cls 去重，classesOf 映射名）
    function eventRow(j) {
        const row = h('div', 'lrow ev-row');
        row.append(
            h('span', 'mono dim', fmtMs(j.ts)),
            h('span', 'mono', `cam${j.id}`),
            h('span', 'dim', camName[j.id] || '—'),  // 名称（列表预取，未命中显示占位）
            h('span', 'ev-algo', j.algo),
            // 规则类型：event_type 由 engine 按 {algo}_{rule_type} 兜底生成，
            // 尾段推导 region_in/dwell；自定义 event_type 不匹配时显示原值
            h('span', 'ev-rule', j.type.endsWith('_region_in')
                ? '进入报警'
                : j.type.endsWith('_dwell') ? '停留报警' : j.type),
            j.snap ? h('span', 'ok-txt', '有快照') : h('span', 'dim', '无快照'));
        // class 芯片：targets 的触发类别去重（每类一个，颜色随 classOf 主题）
        const seen = new Set();
        for (const t of j.targets || []) {
            if (seen.has(t.cls)) continue;
            seen.add(t.cls);
        }
        const sub = h('div', 'lrow-sub ev-sub');
        if (seen.size) {
            sub.innerHTML = [...seen].map((c) =>
                `<span class="chip" style="border-color:${colorOf(c)}">${
                    classesOf[c] ?? `cls${c}`}</span>`).join('');
            row.appendChild(sub);
        }
        row.onclick = () => eventDetail(j);
        return row;
    }

    // 历史检索：from/to 为 datetime-local → ms（db.list_events 的 from/to 单位 ms）
    const dloc2ms = (v) => (v ? new Date(v).getTime() : 0);
    async function query() {
        tri(histList, 'loading');
        const evs = await api.listEvents({
            camera_id: Number(selCam.value) || 0,
            algo_id: inpAlgo.value.trim(),
            from: dloc2ms(inpFrom.value),
            to: dloc2ms(inpTo.value),
            limit: 100,
        });
        histList.innerHTML = '';
        if (!evs.length) { tri(histList, 'empty', { emptyText: '无匹配事件' }); return; }
        for (const e of evs) {
            // DB 列名 → WS 字段名对齐：algo_id→algo、camera_id→id（eventRow 与
            // 详情弹窗都按 WS 契约取 j.algo/j.id；j.id 原是事件行 id，直接用会
            // 把"cam+事件id"错显成摄像头）
            histList.appendChild(eventRow({
                ...e,
                id: e.camera_id,
                algo: e.algo_id,
                ts: e.ts,
                snap: e.snapshot,
            }));
        }
    }
    qBtn.onclick = () => query().catch(() => {});
    query();

    // WS 实时头插：新事件过当前筛选条件 → 列表顶部插入（上限 100 与查询一致）。
    // businessd 落库先于 WS 广播，头插的事件已入库，查询中的竞态由随后的
    // query() 结果自然收敛，无需加锁。
    // 快照 note：截图在事件广播后异步落盘（~0.5s），WS 行时刻快照未生成 →
    // 显示"无快照"；重新查询即取到 DB 回填的真实路径
    function matchesFilters(j) {
        if (Number(selCam.value) !== 0 && j.id !== Number(selCam.value)) return false;
        const q = inpAlgo.value.trim();
        if (q && !(j.algo || '').includes(q)) return false;
        const from = dloc2ms(inpFrom.value), to = dloc2ms(inpTo.value);
        if (from && j.ts < from) return false;
        if (to && j.ts > to) return false;
        return true;
    }
    // 快照异步回填：businessd 截图在事件广播后 ~0.5s 才落盘（板上实测
    // 0.25~0.76s），WS 行时刻 DB 还没有快照路径（engine 事件帧不带 snap）。
    // 2s 后按 cam+时间窗静默单查，按 ts 精确命中即就地替换该行——不重建
    // 列表、不打断浏览；仍无快照（截图失败/流断）保持"无快照"，下次查询兜底
    function refreshRowSnap(j, row) {
        setTimeout(async () => {
            try {
                const evs = await api.listEvents({
                    camera_id: j.id, from: j.ts - 1000, to: j.ts + 1000, limit: 10,
                }, { silent: true });
                const hit = evs.find((e) => e.ts === j.ts);
                if (hit && hit.snapshot && row.isConnected) {
                    row.replaceWith(eventRow({ ...j, snap: hit.snapshot }));
                }
            } catch (_) { /* 静默：回填失败不扰用户 */ }
        }, 2000);
    }
    const offLive = onEvent('event', (j) => {
        if (!liveChk.checked) return;  // 实时开关关闭：只存查询态，不做头插
        if (!matchesFilters(j)) return;
        if (histList.firstElementChild &&
            histList.firstElementChild.className === 'tri') {
            histList.innerHTML = '';  // 空态占位 → 有真实事件进来了，清占位
        }
        const row = eventRow(j);
        histList.prepend(row);
        while (histList.children.length > 100) histList.lastChild.remove();
        if (!j.snap) refreshRowSnap(j, row);
    });

    function eventDetail(j) {
        const root = document.getElementById('modal-root');
        const mask = h('div', 'mask');
        const tg = (j.targets || []).map((t) =>
            `#${t.track_id} ${classesOf[t.cls] ?? t.cls} ${Math.round(t.conf * 100)}%`).join('、');
        // 快照：businessd 截图落 recordings/snap/ → /recordings 挂载直出；
        // DB 存绝对路径，HTTP url 取文件名段（basename）
        const snapUrl = j.snap
            ? '/recordings/snap/' + String(j.snap).split('/').pop()
            : null;
        mask.innerHTML = `<div class="modal"><div class="m-title">${j.type} @cam${j.id}（${j.algo}）</div>
            <div class="m-body">
            ${snapUrl ? `<img src="${snapUrl}" alt="快照"
                style="max-width:100%;border:1px solid var(--line2);border-radius:4px;"
                onerror="this.outerHTML='<div class=\\'dim\\'>快照文件已清理</div>'">`
                      : '<div class="dim">无快照</div>'}
            <div class="mono dim small">${tg || '无目标'}</div>
            <div class="mono dim small">${fmtMs(j.ts)}</div></div>
            <div class="m-btns"><button class="btn" id="d-ok">关闭</button></div></div>`;
        mask.querySelector('#d-ok').onclick = () => mask.remove();
        mask.addEventListener('click', (e) => { if (e.target === mask) mask.remove(); });
        root.appendChild(mask);
    }

    return { unmount() { offLive(); } };
}

// ============================================================ #/recordings
async function pageRecordings() {
    const wrap = h('div', 'page');
    wrap.appendChild(h('h2', '', '录像回放'));
    const f = h('div', 'filters');
    const selCam = document.createElement('select'); selCam.className = 'sel';
    selCam.appendChild(new Option('全部摄像头', '0'));
    const inpFrom = h('input', 'inp'); inpFrom.type = 'datetime-local';
    const inpTo = h('input', 'inp'); inpTo.type = 'datetime-local';
    const qBtn = h('button', 'btn btn-acc', '查询');
    f.append(selCam, inpFrom, inpTo, qBtn);
    const list = h('div', 'list');
    const player = document.createElement('video');
    player.className = 'rec-player'; player.controls = true;
    player.disablePictureInPicture = true;  // 回放同样禁画中画
    player.style.display = 'none';
    wrap.append(f, list, player);
    $view().appendChild(wrap);

    try {
        for (const c of await api.listCameras({ silent: true })) {
            selCam.appendChild(new Option(`cam${c.id} ${c.name}`, String(c.id)));
        }
    } catch (_) {}

    async function query() {
        tri(list, 'loading');
        const r = await api.listRecordings({
            camera_id: Number(selCam.value) || 0,
            from: inpFrom.value ? new Date(inpFrom.value).getTime() : 0,   // ms（start_ts=ZLM 秒×1000）
            to: inpTo.value ? new Date(inpTo.value).getTime() : 0,
            limit: 100,
        });
        list.innerHTML = '';
        if (!r.items || !r.items.length) { tri(list, 'empty', { emptyText: '无录像段（ZLM on_record_mp4 需开启）' }); return; }
        list.appendChild(h('div', 'dim small pad', `共 ${r.total} 段`));
        for (const s of r.items) {
            const row = h('div', 'lrow');
            const play = h('button', 'btn btn-acc', '▶ 播放');
            play.onclick = () => {
                player.style.display = 'block';
                player.src = s.url;      // MP4 直链（businessd /recordings 静态挂载）
                player.play().catch(() => {});
                player.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
            };
            row.append(
                h('span', 'mono', `cam${s.camera_id}`),
                h('span', 'mono', fmtMs(s.start_ts)),
                h('span', 'mono', `${Math.round(s.duration_s)}s`),
                play);
            list.appendChild(row);
        }
    }
    qBtn.onclick = () => query().catch(() => {});
    query();
    return { unmount() { player.pause(); player.removeAttribute('src'); } };
}

// ============================================================ #/settings
async function pageSettings() {
    const wrap = h('div', 'page');
    wrap.appendChild(h('h2', '', '系统设置'));
    const info = h('div', 'panel');
    const res = h('div', 'panel');
    wrap.append(info, res, h('div', 'dim small pad', '网络 / 存储 / 云对接 / OTA：后续里程碑'));
    $view().appendChild(wrap);

    // 占位字段容错：0 值一律显示 "--"（disk 后端 M6 才接实值，前端设计 §5.7）
    const gb = (v) => (v && v > 0 ? `${v.toFixed(1)} GB` : '--');
    const pct = (v) => (Number.isFinite(v) && v >= 0 ? `${v.toFixed(0)}%` : '--');
    // 三色分级：正常青 / ≥60% 橙 / ≥85% 红（工控告警阈值惯例）
    const grade = (v) => (v >= 85 ? 'crit' : v >= 60 ? 'warn' : '');

    // 资源卡：label + 百分比 + 进度条 + 副信息
    function resCard(label, valPct, sub, valText) {
        const g = grade(valPct);
        const card = h('div', 'res-card');
        const head = h('div', 'res-head');
        head.appendChild(h('span', 'dim small', label));
        head.appendChild(h('span', `res-val ${g}`, valText !== undefined ? valText : pct(valPct)));
        card.appendChild(head);
        const bar = h('div', 'res-bar');
        const fill = h('div', `res-fill ${g}`);
        fill.style.width = `${Math.max(0, Math.min(100, valPct || 0))}%`;
        bar.appendChild(fill);
        card.appendChild(bar);
        if (sub) card.appendChild(h('div', 'res-sub', sub));
        return card;
    }

    async function reload() {
        const [inf, r] = await Promise.all([
            api.systemInfo({ silent: true }).catch(() => null),
            api.getResources({ silent: true }).catch(() => null),
        ]);
        info.innerHTML = '';
        info.append(
            h('div', 'krow', `版本 ${inf ? inf.version : '--'}　ready ${inf && inf.ready ? '✓' : '…'}　uptime ${inf ? fmtUptime(inf.uptime_s) : '--'}`),
            h('div', 'krow', `摄像头在线 ${inf ? `${inf.cameras_online}/${inf.cameras_total}` : '--'}　降级 L${degradeLevel}`));
        const disk = inf && inf.disk ? inf.disk : {};

        res.innerHTML = '';
        res.appendChild(h('h3', '', '资源监控'));
        if (!r || r.engine_online !== true) {
            res.appendChild(h('div', 't-err', 'engine 离线（NPU/队列指标不可用）'));
        }
        // —— 五张系统/硬件指标卡（engine 离线时 CPU/内存/磁盘仍可用：businessd 自采）——
        const grid = h('div', 'res-grid');
        const npu = r && r.npu ? r.npu : {};
        const mem = r && r.mem ? r.mem : {};
        grid.appendChild(resCard('CPU 占用', r ? r.cpu_pct : -1));
        grid.appendChild(resCard('内存占用', mem.used_pct,
            mem.total_mb > 0 ? `总量 ${mem.total_mb} MB` : ''));
        grid.appendChild(resCard('存储占用', disk.used_pct,
            disk.total_gb > 0 ? `总量 ${gb(disk.total_gb)}（可用 ${gb(disk.free_gb)}）` : ''));
        grid.appendChild(resCard('NPU 负载', npu.load_pct,
            `steady核 ${(npu.steady_cores || []).join(',') || '--'} · triggered核 ${(npu.triggered_cores || []).join(',') || '--'}`));
        grid.appendChild(resCard('NPU 温度', r && r.temp_c >= 0 ? r.temp_c : -1,
            '降级 L4 阈值 85°C', r && r.temp_c >= 0 ? `${r.temp_c.toFixed(0)}°C` : '--'));
        res.appendChild(grid);

        // —— 队列/管道明细（等宽行保留）——
        const det = h('div', 'krow mono dim',
            `队列 steady=${r && r.queue ? r.queue.steady_depth : '--'} triggered=${r && r.queue ? r.queue.triggered_depth : '--'} uds_drop=${r && r.queue ? r.queue.uds_dropped : '--'}　DDR=${r ? pct(r.ddr_pct) : '--'}`);
        res.appendChild(det);
        const cams = (r && r.cams) || {};
        for (const [id, c] of Object.entries(cams)) {
            const dec = c.dec || {};
            const samp = c.samp || {};
            res.appendChild(h('div', 'krow mono',
                `cam${id}：dec ${dec.in || 0}/${dec.out || 0}/drop${dec.drop || 0}　samp in=${samp.in || 0} sampled=${samp.sampled || 0}`));
        }
    }
    reload();
    const timer = setInterval(reload, 3000);  // 3s：CPU 差分窗口 + 首个周期即出数
    return { unmount() { clearInterval(timer); } };
}

// ============================================================ boot
// DOM 守卫：node 导入安全（模块测试防语法朽坏），浏览器中正常 boot
if (typeof document !== 'undefined') {
    loadClasses();
    bootTopbar();
    // 无 hash：赋值即触发 hashchange → route（勿再显式调用，防双渲染）
    if (!location.hash) location.hash = '#/grid';
    else route();
}
