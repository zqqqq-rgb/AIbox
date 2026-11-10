import { api, setToken } from './api.js';
import { h } from './ui.js';

// The configured device credential is exchanged for an HttpOnly session cookie.
// Never persist the login credential in localStorage or include it in a URL.
export function installAccessGate({ onAuthenticated, onExpired }) {
    let gate = null;
    let checking = false;
    let authenticated = false;
    let generation = 0;
    let messageNode = null;
    const button = document.getElementById('access-logout');

    function showGate(message = '') {
        if (gate) { if (messageNode) messageNode.textContent = message; return; }
        onExpired();
        authenticated = false;
        if (button) button.hidden = true;
        gate = h('div', 'access-gate');
        const form = h('form', 'panel access-card');
        form.append(h('h1', '', '登录 AIBox'), h('p', 'dim', '输入设备管理员配置的访问凭据。'));
        const label = h('label', '', '访问凭据');
        const input = h('input');
        input.type = 'password'; input.autocomplete = 'current-password'; input.required = true;
        input.name = 'credential'; label.appendChild(input);
        const error = h('p', 'warn-txt', message); error.setAttribute('role', 'alert'); messageNode = error;
        const submit = h('button', 'btn btn-acc', '登录'); submit.type = 'submit';
        const retry = h('button', 'btn', '重新连接'); retry.type = 'button'; retry.onclick = () => check();
        form.append(label, error, submit, retry); gate.appendChild(form); document.body.appendChild(gate);
        form.onsubmit = async (event) => {
            event.preventDefault();
            if (!input.value || submit.disabled) return;
            submit.disabled = true; error.textContent = '';
            try {
                await api.login(input.value);
                input.value = ''; setToken('');
                await check();
            } catch (failure) {
                error.textContent = failure.message || '登录失败，请检查凭据或连接';
                input.select();
            } finally { submit.disabled = false; }
        };
        input.focus();
    }

    async function check() {
        if (checking) return;
        checking = true;
        const current = ++generation;
        try {
            const status = await api.authStatus();
            if (current !== generation) return;
            if (status.auth_required && !status.authenticated) { authenticated = false; showGate(); return; }
            if (!authenticated) {
                authenticated = true;
                await onAuthenticated(() => current === generation && authenticated);
            }
            if (current !== generation) return;
            gate?.remove(); gate = null; messageNode = null;
            if (button) button.hidden = !status.auth_required;
        } catch (failure) {
            if (current === generation) {
                authenticated = false;
                showGate(`设备暂不可达：${failure.message}`);
            }
        } finally { if (current === generation) checking = false; }
    }

    const expired = () => { ++generation; checking = false; authenticated = false; showGate('登录已失效，请重新登录'); };
    window.addEventListener('aibox-auth-required', expired);
    if (button) button.onclick = async () => {
        button.disabled = true;
        try { await api.logout(); setToken(''); expired(); }
        catch (failure) { showGate(`退出未确认：${failure.message}`); }
        finally { button.disabled = false; }
    };
    check();
    return { check, destroy() { ++generation; gate?.remove(); window.removeEventListener('aibox-auth-required', expired); } };
}
