// api.js —— REST 封装（前端设计 §4.1）：envelope 解包、错误码→toast、Bearer 预留
// 后端契约：{"code":0,"msg":"ok","data":...}；错误码表详设 §8.2
import { toast } from './ui.js';

const TIMEOUT_MS = 8000;  // 板上 ZLM 接口实测 >2s，放宽（前端设计 §4.1）

// 错误码兜底文案（优先用后端 msg，空则按此表）
const ERR_TEXT = {
    1001: '参数错误',
    1002: '对象不存在',
    1003: '状态不允许（通道配置中/未运行）',
    2001: 'RTSP 探测失败',
    2002: 'RTSP 账密错误',
    2003: '编码不受支持',
    3001: 'NPU 算力预检超限，请降低检测帧率',
    3002: 'NPU 内存预检超限',
    3003: '算法包校验失败（签名/版本/平台）',
    3004: '新版本自检失败，已自动回滚',
    4001: '协议错误',
    4002: 'ZLM 编排失败',
    5001: '内部错误',
};

export class ApiError extends Error {
    constructor(code, msg, data) {
        super(msg || ERR_TEXT[code] || `错误码 ${code}`);
        this.code = code;
        this.data = data;
    }
}

// token 预留：localStorage 存取；空 = 不发（首版局域网无鉴权，详设 §5.4）
export function getToken() {
    try { return localStorage.getItem('aibox_token') || ''; } catch (_) { return ''; }
}
export function setToken(t) {
    try { t ? localStorage.setItem('aibox_token', t) : localStorage.removeItem('aibox_token'); }
    catch (_) { /* node/隐私模式 */ }
}

async function request(method, path, body, { silent = false, timeout = TIMEOUT_MS, authEvent = true, withBearer = true } = {}) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeout);
    const headers = { 'Content-Type': 'application/json' };
    const tk = getToken();
    if (tk && withBearer) headers['Authorization'] = `Bearer ${tk}`;
    let j;
    try {
        const res = await fetch(path, {
            method, headers,
            credentials: 'same-origin',
            body: body !== undefined ? JSON.stringify(body) : undefined,
            signal: ctrl.signal,
        });
        if (res.status === 401 || res.status === 403) {
            if (authEvent && typeof window !== 'undefined')
                window.dispatchEvent(new Event('aibox-auth-required'));
            throw new ApiError(res.status, res.status === 401 ? '请登录后继续操作' : '当前请求未获授权');
        }
        if (res.status === 404 && path === '/api/auth/status')
            throw new ApiError(404, '页面与后端版本不一致，请重新编译并重启 AIBox 服务');
        try {
            j = await res.json();
        } catch (_) {
            const message = `服务返回了非 JSON 响应${res.status ? `（HTTP ${res.status}）` : ''}，请检查服务状态`;
            if (!silent) toast(message, 'err');
            throw new ApiError(5001, message);
        }
    } catch (e) {
        if (e instanceof ApiError) throw e;
        if (!silent) toast(`请求失败：${path}`, 'err');
        throw new ApiError(5001, `请求失败：${e.name === 'AbortError' ? '超时' : e.message}`);
    } finally {
        clearTimeout(timer);
    }
    if (!j || typeof j.code !== 'number') {
        if (!silent) toast(`响应格式异常：${path}`, 'err');
        throw new ApiError(5001, '响应缺少 code 字段');
    }
    if (j.code !== 0) {
        const err = new ApiError(j.code, j.msg, j.data);
        if (!silent) toast(j.data?.pending ? '配置已保存，等待设备执行' : err.message,
            j.data?.pending || j.code === 3004 ? 'warn' : 'err');
        throw err;
    }
    return j.data;
}

async function configuration(method, path, body) {
    try { return await request(method, path, body); }
    catch (error) {
        if (error instanceof ApiError && error.code === 1003 && error.data?.accepted && error.data.pending)
            return error.data;
        throw error;
    }
}

export const api = {
    // 摄像头
    listCameras: () => request('GET', '/api/cameras'),
    addCamera: (b) => configuration('POST', '/api/cameras', b),
    // 网络扫描（WS-Discovery ~3s）与 ONVIF 取流（GetProfiles+GetStreamUri ≤3 次往返）
    scanInterfaces: () => request('GET', '/api/cameras/scan/interfaces'),
    scanCameras: (options = {}) => request('POST', '/api/cameras/scan', options, { timeout: 15000 }),
    getOnvifUris: (b) => request('POST', '/api/cameras/onvif_uri', b, { timeout: 15000 }),
    updateCamera: (id, b) => configuration('PUT', `/api/cameras/${id}`, b),
    updateAlgos: (id, algos) => configuration('PUT', `/api/cameras/${id}/algos`, { algos }),
    deleteCamera: (id) => configuration('DELETE', `/api/cameras/${id}`),
    getOperation: (id) => request('GET', `/api/operations/${encodeURIComponent(id)}`, undefined, { silent: true }),
    getStream: (id, quality = 'sub', opt) =>
        request('GET', `/api/cameras/${id}/stream?quality=${quality}`, undefined, opt),
    // ROI 编辑底图（二进制 JPEG，直接作 <img src>；时间戳防缓存）
    snapshotUrl: (id) => `/api/cameras/${id}/snapshot?t=${Date.now()}`,
    // 算法
    listAlgorithms: () => request('GET', '/api/algorithms'),
    listTasks: (opt) => request('GET', '/api/tasks', undefined, opt),
    getTask: (id) => request('GET', `/api/tasks/${id}`),
    saveTask: (id, body) => request(id ? 'PUT' : 'POST', id ? `/api/tasks/${id}` : '/api/tasks', body),
    deleteTask: (id, revision) => request('DELETE', `/api/tasks/${id}`, { revision }),
    setTaskEnabled: (id, revision, enabled) => configuration('PUT', `/api/tasks/${id}/enabled`, { revision, enabled }),
    listCapabilities: () => request('GET', '/api/capabilities'),
    previewTasks: (camera_id) => request('POST', '/api/tasks/preview', { camera_id, include_drafts: true }),
    rollbackAlgo: (id) => request('POST', `/api/algorithms/${id}/rollback`, undefined, { silent: true }),
    // 事件 / 录像（from/to 单位均为 ms——db 层 start_ts=ZLM 秒×1000，events.ts=ms）
    listEvents: (q, opt) => request('GET', '/api/events?' + new URLSearchParams(q), undefined, opt),
    getEvent: (id, opt) => request('GET', `/api/events/${encodeURIComponent(id)}`, undefined, opt),
    listRecordings: (q) => request('GET', '/api/recordings?' + new URLSearchParams(q)),
    // 系统
    getResources: (opt) => request('GET', '/api/resources', undefined, opt),
    systemInfo: (opt) => request('GET', '/api/system/info', undefined, opt),
    authStatus: () => request('GET', '/api/auth/status', undefined, { silent: true, authEvent: false, withBearer: false }),
    login: (token) => request('POST', '/api/auth/login', { token }, { silent: true, authEvent: false, withBearer: false }),
    logout: () => request('POST', '/api/auth/logout', {}, { silent: true, authEvent: false, withBearer: false }),
};
