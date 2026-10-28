// player.js —— CamPlayer 播放内核（详设 §6.2 / 前端设计 §5.1）
// ZLMRTCClient（WebRTC）封装 + 指数退避重连（1s→2s→4s→8s）
// 信令：GET /api/cameras/{id}/stream?quality= → signaling 已是完整 URL
//（含 ?app=live&stream=camN_x&type=play），直接作 zlmsdpUrl
//
// 用法：
//   const p = new CamPlayer(videoEl, camId, {quality:'sub', audio:false,
//                                           onState: s => {...}});  // 'connecting'|'playing'|'retry'|'failed'
//   await p.start(); ... p.destroy();
import { api } from './api.js';

const BACKOFF_MS = [1000, 2000, 4000, 8000];

export class CamPlayer {
    // videoEl: <video> 元素（须 muted playsinline autoplay 属性由调用方设置）
    constructor(videoEl, camId, { quality = 'sub', audio = false, onState = null } = {}) {
        this.video = videoEl;
        this.camId = camId;
        this.quality = quality;
        this.audio = audio;
        this.onState = onState;
        this.pc = null;
        this.attempts = 0;
        this.timer = 0;
        this.destroyed = false;
        this.signaling = '';
    }

    async start() {
        if (typeof ZLMRTCClient === 'undefined') {
            this._emit('failed');
            throw new Error('ZLMRTCClient 未加载（web/vendor/ZLMRTCClient.js）');
        }
        this._emit('connecting');
        try {
            const s = await api.getStream(this.camId, this.quality, { silent: true });
            this.signaling = s.signaling;
        } catch (e) {
            if (this.destroyed) return;
            this._scheduleRetry();
            return;
        }
        if (this.destroyed) return;
        this._createPeer();
    }

    _createPeer() {
        this._teardownPeer();
        try {
            // eslint-disable-next-line no-undef
            this.pc = new ZLMRTCClient.Endpoint({
                element: this.video,
                debug: false,
                zlmsdpUrl: this.signaling,
                simulcast: false,
                useCamera: false,   // 播放端不采集
                audioEnable: this.audio,
                videoEnable: true,
                recvOnly: true,
            });
        } catch (e) {
            this._scheduleRetry();
            return;
        }
        const E = ZLMRTCClient.Events;
        this.pc.on(E.WEBRTC_ON_REMOTE_STREAMS, () => {
            this.attempts = 0;
            this._emit('playing');
        });
        this.pc.on(E.WEBRTC_ON_CONNECTION_STATE_CHANGE, (st) => {
            if (st === 'connected' || st === 'completed') return;  // 播放态由 REMOTE_STREAMS 发
            if (st === 'failed' || st === 'disconnected' || st === 'closed') this._scheduleRetry();
        });
        this.pc.on(E.WEBRTC_OFFER_ANWSER_EXCHANGE_FAILED, () => this._scheduleRetry());
        this.pc.on(E.WEBRTC_ICE_CANDIDATE_ERROR, () => this._scheduleRetry());
        this.pc.on(E.WEBRTC_NOT_SUPPORT, () => { this._emit('failed'); });
    }

    _scheduleRetry() {
        if (this.destroyed || this.timer) return;
        this._teardownPeer();
        const ms = BACKOFF_MS[Math.min(this.attempts, BACKOFF_MS.length - 1)];
        this.attempts += 1;
        this._emit('retry');
        this.timer = setTimeout(() => {
            this.timer = 0;
            if (this.destroyed) return;
            if (!this.signaling) { this.start(); return; }
            this._createPeer();
        }, ms);
    }

    _teardownPeer() {
        if (this.pc) {
            try { this.pc.close(); } catch (_) {}
            this.pc = null;
        }
    }

    // 播放器重建后 overlay 须 reset 重锚（调用方在 onState('playing') 里做）
    destroy() {
        this.destroyed = true;
        if (this.timer) { clearTimeout(this.timer); this.timer = 0; }
        this._teardownPeer();
    }

    _emit(s) { if (this.onState) this.onState(s); }
}
