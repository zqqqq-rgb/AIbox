// ws.js —— WS 单例管理（前端设计 §4.4）
//   /ws/events      常驻：全局事件分发（event/camera_status/degrade/registry_ready）
//   /ws/meta/{cam}  租借：按页面引用计数（同一 cam 多 viewer 共享一条连接，
//                   后端已做 sub_meta 去重，前端计数防重复订阅）
// 重连：指数退避 1s→2s→4s→8s 封顶；连接状态经 onConnState/onState 通知
//（页面据此 overlay.reset() 重新锚定——锚定只发生一次，重连必须重锚）

const BACKOFF_MS = [1000, 2000, 4000, 8000];

function wsUrl(port, path) {
    const host = location.hostname || '127.0.0.1';
    const p = Number(port) || 8081;
    return `ws://${host}:${p}${path}`;
}

// ---- 通用连接器：指数退避重连 ----
function connectLoop(url, { onOpen, onMsg, onLost }) {
    let ws = null;
    let attempt = 0;
    let timer = 0;
    let closed = false;
    function open() {
        if (closed) return;
        try { ws = new WebSocket(url); } catch (_) { retry(); return; }
        ws.onopen = () => { attempt = 0; if (onOpen) onOpen(); };
        ws.onmessage = (e) => { if (onMsg) onMsg(e.data); };
        ws.onclose = () => { if (onLost) onLost(); retry(); };
        ws.onerror = () => { try { ws.close(); } catch (_) {} };
    }
    function retry() {
        if (closed) return;
        const ms = BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)];
        attempt += 1;
        timer = setTimeout(open, ms);
    }
    open();
    return {
        close() {
            closed = true;
            clearTimeout(timer);
            if (ws) { ws.onclose = null; try { ws.close(); } catch (_) {} }
        },
    };
}

// ================ /ws/events（常驻单例） ================
const evListeners = new Map();      // evt 名 → Set<fn>
const connListeners = new Set();    // 'connected'|'lost' → fn
let evConn = null;
let evPort = 8081;

export function startEvents(port) {
    if (Number(port)) evPort = Number(port);
    if (evConn) return;
    evConn = connectLoop(wsUrl(evPort, '/ws/events'), {
        onOpen: () => connListeners.forEach((fn) => fn('connected')),
        onMsg: (line) => {
            let j;
            try { j = JSON.parse(line); } catch (_) { return; }
            const set = evListeners.get(j.evt);
            if (set) set.forEach((fn) => { try { fn(j, line); } catch (e) { console.error(e); } });
        },
        onLost: () => connListeners.forEach((fn) => fn('lost')),
    });
}

export function stopEvents() {
    if (evConn) { evConn.close(); evConn = null; }
    evListeners.clear();
    connListeners.clear();
}

// 注册事件监听，返回取消函数。evt 名 = engine 原始行的 "evt" 字段
export function onEvent(evt, fn) {
    let set = evListeners.get(evt);
    if (!set) { set = new Set(); evListeners.set(evt, set); }
    set.add(fn);
    return () => set.delete(fn);
}

// 全局连接状态（WS 断线横幅）
export function onConnState(fn) {
    connListeners.add(fn);
    return () => connListeners.delete(fn);
}

// ================ /ws/meta/{cam}（引用计数租借） ================
// metaRefs: cam → { conn, refs: Map<token, {onMsg, onState}> }
const metaRefs = new Map();
let metaSeq = 0;

export function acquireMeta(cam, handlers) {
    const token = ++metaSeq;
    let slot = metaRefs.get(cam);
    if (!slot) {
        slot = { conn: null, refs: new Map(), connected: false };
        metaRefs.set(cam, slot);
        const notify = (s) => slot.refs.forEach((h) => h.onState && h.onState(s));
        slot.conn = connectLoop(wsUrl(evPort, `/ws/meta/${cam}`), {
            onOpen: () => { slot.connected = true; notify('connected'); },
            onMsg: (line) => {
                let j;
                try { j = JSON.parse(line); } catch (_) { return; }
                slot.refs.forEach((h) => h.onMsg && h.onMsg(j, line));
            },
            onLost: () => { slot.connected = false; notify('lost'); },
        });
    }
    slot.refs.set(token, handlers);
    if (slot.connected && handlers.onState) handlers.onState('connected');
    return function release() {
        const s = metaRefs.get(cam);
        if (!s) return;
        s.refs.delete(token);
        if (s.refs.size === 0) {
            s.conn.close();
            metaRefs.delete(cam);
        }
    };
}
