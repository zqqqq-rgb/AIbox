import { afterEach, describe, expect, it, vi } from 'vitest';
vi.mock('../ui.js', () => ({ toast: vi.fn() }));
import { api, ApiError } from '../api.js';

afterEach(() => vi.unstubAllGlobals());
function reply(value) {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ json: async () => value }));
}
describe('durable configuration responses', () => {
    it('retains accepted multi-camera task operations while confirmation is pending', async () => {
        const data = { accepted: true, pending: true, revision: 2, applied: false, operations: [{ camera_id: 1, operation_id: 'cfg-1' }] };
        reply({ code: 1003, msg: 'saved', data });
        expect(await api.setTaskEnabled(8, 1, true)).toEqual(data);
        expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ revision: 1, enabled: true });
    });
    it('rejects task revision conflicts instead of treating them as saved', async () => {
        reply({ code: 1003, msg: 'revision conflict', data: { accepted: false } });
        await expect(api.setTaskEnabled(8, 1, true)).rejects.toMatchObject({ code: 1003 });
    });
    it('preserves accepted operation metadata without treating it as applied', async () => {
        const data = { accepted: true, pending: true, operation_id: 'cfg-1', apply_status: 'retry_wait' };
        reply({ code: 1003, msg: 'pending', data });
        expect(await api.updateAlgos(1, [])).toEqual(data);
    });
    it('continues rejecting busy requests with no accepted operation', async () => {
        const data = { accepted: false, pending: false };
        reply({ code: 1003, msg: 'busy', data });
        await expect(api.addCamera({})).rejects.toMatchObject({ code: 1003, data });
    });
    it('keeps terminal failures as errors and retains their operation id', async () => {
        const data = { accepted: true, pending: false, operation_id: 'cfg-2', apply_status: 'failed' };
        reply({ code: 1002, msg: 'unavailable', data });
        await expect(api.updateCamera(1, {})).rejects.toMatchObject({ code: 1002, data });
        expect(new ApiError(1003, 'busy', data).data).toEqual(data);
    });
    it('does not show a superseded accepted operation as saved or still pending', async () => {
        const data = { accepted: true, pending: false, operation_id: 'cfg-old', apply_status: 'superseded' };
        reply({ code: 1003, msg: 'replaced by a newer target', data });
        await expect(api.updateAlgos(1, [])).rejects.toMatchObject({ code: 1003, data });
    });
});

describe('session authentication', () => {
    it('explains a missing auth endpoint as a backend version mismatch', async () => {
        const json = vi.fn();
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 404, json }));
        await expect(api.authStatus()).rejects.toMatchObject({ code: 404, message: expect.stringContaining('版本不一致') });
        expect(json).not.toHaveBeenCalled();
    });
    it('reports empty or non-JSON responses without leaking a JSON parser exception', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 502, json: async () => { throw new SyntaxError('Unexpected end of JSON input'); } }));
        await expect(api.systemInfo({ silent: true })).rejects.toMatchObject({ code: 5001, message: expect.stringContaining('HTTP 502') });
    });
    it('classifies expired sessions before trying to parse an HTML proxy response', async () => {
        const json = vi.fn();
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 401, json }));
        await expect(api.listCameras()).rejects.toMatchObject({ code: 401 });
        expect(json).not.toHaveBeenCalled();
    });
    it('exchanges the login credential in the body with same-origin cookies', async () => {
        reply({ code: 0, data: { authenticated: true } });
        await api.login('test-credential');
        const [url, options] = fetch.mock.calls[0];
        expect(url).toBe('/api/auth/login');
        expect(options.credentials).toBe('same-origin');
        expect(JSON.parse(options.body)).toEqual({ token: 'test-credential' });
        expect(url).not.toContain('credential');
    });
    it('passes explicit scan options to the existing discovery endpoint', async () => {
        reply({ code: 0, data: { devices: [] } });
        await api.scanCameras({ interface: 'eth0', cidr: '192.168.1.0/24', timeout_ms: 5000 });
        expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ interface: 'eth0', cidr: '192.168.1.0/24', timeout_ms: 5000 });
    });
});
