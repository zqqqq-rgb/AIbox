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
    constructor(code, msg) {
        super(msg || ERR_TEXT[code] || `错误码 ${code}`);
        this.code = code;
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

async function request(method, path, body, { silent = false, timeout = TIMEOUT_MS } = {}) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeout);
    const headers = { 'Content-Type': 'application/json' };
    const tk = getToken();
    if (tk) headers['Authorization'] = `Bearer ${tk}`;
    let j;
    try {
        const res = await fetch(path, {
            method, headers,
            body: body !== undefined ? JSON.stringify(body) : undefined,
            signal: ctrl.signal,
        });
        j = await res.json();
    } catch (e) {
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
        const err = new ApiError(j.code, j.msg);
        if (!silent) toast(err.message, j.code === 3004 ? 'warn' : 'err');
        throw err;
    }
    return j.data;
}

export const api = {
    // 摄像头
    listCameras: () => request('GET', '/api/cameras'),
    addCamera: (b) => request('POST', '/api/cameras', b),
    // 网络扫描（WS-Discovery ~3s）与 ONVIF 取流（GetProfiles+GetStreamUri ≤3 次往返）
    scanCameras: () => request('POST', '/api/cameras/scan', {}, { timeout: 15000 }),
    getOnvifUris: (b) => request('POST', '/api/cameras/onvif_uri', b, { timeout: 15000 }),
    updateCamera: (id, b) => request('PUT', `/api/cameras/${id}`, b),
    updateAlgos: (id, algos) => request('PUT', `/api/cameras/${id}/algos`, { algos }),
    deleteCamera: (id) => request('DELETE', `/api/cameras/${id}`),
    getStream: (id, quality = 'sub', opt) =>
        request('GET', `/api/cameras/${id}/stream?quality=${quality}`, undefined, opt),
    // ROI 编辑底图（二进制 JPEG，直接作 <img src>；时间戳防缓存）
    snapshotUrl: (id) => `/api/cameras/${id}/snapshot?t=${Date.now()}`,
    // 算法
    listAlgorithms: () => request('GET', '/api/algorithms'),
    rollbackAlgo: (id) => request('POST', `/api/algorithms/${id}/rollback`, undefined, { silent: true }),
    // 事件 / 录像（from/to 单位均为 ms——db 层 start_ts=ZLM 秒×1000，events.ts=ms）
    listEvents: (q, opt) => request('GET', '/api/events?' + new URLSearchParams(q), undefined, opt),
    listRecordings: (q) => request('GET', '/api/recordings?' + new URLSearchParams(q)),
    // 系统
    getResources: (opt) => request('GET', '/api/resources', undefined, opt),
    systemInfo: (opt) => request('GET', '/api/system/info', undefined, opt),
};
