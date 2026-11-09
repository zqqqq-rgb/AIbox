import { describe, it, expect } from 'vitest';
import { websocketUrl, scanRequest } from '../connection_model.js';

describe('remote and LAN websocket routing', () => {
    const local = { protocol: 'http:', host: '192.168.1.20:8080', hostname: '192.168.1.20' };
    const remote = { protocol: 'https:', host: 'aibox.example.com:8443', hostname: 'aibox.example.com' };
    it('preserves the existing independent LAN websocket port', () => {
        expect(websocketUrl({ ws_port: 8081 }, '/ws/events', local)).toBe('ws://192.168.1.20:8081/ws/events');
    });
    it('uses the HTTPS entry port for both event and detection connections', () => {
        expect(websocketUrl({ ws_port: 8081 }, '/ws/events', remote)).toBe('wss://aibox.example.com:8443/ws/events');
        expect(websocketUrl({ same_origin_ws: true }, '/ws/meta/2', remote)).toBe('wss://aibox.example.com:8443/ws/meta/2');
    });
    it('supports a controlled same-host WSS origin without leaking credentials', () => {
        expect(websocketUrl({ public_ws_url: 'wss://aibox.example.com:9443' }, '/ws/events', remote))
            .toBe('wss://aibox.example.com:9443/ws/events');
        for (const address of ['ws://aibox.example.com', 'wss://other.example.com',
            'wss://token@aibox.example.com', 'wss://aibox.example.com/path', 'wss://aibox.example.com/?token=secret'])
            expect(() => websocketUrl({ public_ws_url: address }, '/ws/events', remote)).toThrow();
    });
    it('formats IPv6 LAN addresses correctly', () => {
        expect(websocketUrl({}, '/ws/events', { ...local, hostname: '::1' })).toBe('ws://[::1]:8081/ws/events');
    });
});

describe('scan request boundaries', () => {
    it('keeps automatic scanning available without a specified subnet', () => {
        expect(scanRequest('', '', '3000')).toEqual({ timeout_ms: 3000 });
    });
    it('requires a selected interface for an explicit scan range', () => {
        expect(() => scanRequest('', '192.168.0.0/24')).toThrow('请选择网卡');
        expect(scanRequest('eth0', ' 192.168.0.0/24 ', 5000))
            .toEqual({ interface: 'eth0', cidr: '192.168.0.0/24', timeout_ms: 5000 });
    });
    it('rejects unbounded or invalid wait times before sending a request', () => {
        for (const timeout of [0, 499, 10001, 'x', 3000.5]) expect(() => scanRequest('', '', timeout)).toThrow();
    });
});
