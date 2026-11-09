export function websocketUrl(config, path, page) {
    const secure = page.protocol === 'https:';
    if (config.public_ws_url) {
        const base = new URL(config.public_ws_url);
        if (base.protocol !== 'wss:' || base.hostname !== page.hostname ||
            base.username || base.password || base.search || base.hash || base.pathname !== '/')
            throw new Error('实时连接地址配置无效');
        return base.origin + path;
    }
    if (config.same_origin_ws || secure)
        return `${secure ? 'wss:' : 'ws:'}//${page.host}${path}`;
    const port = Number(config.ws_port) || 8081;
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('实时连接端口无效');
    const host = page.hostname.includes(':') && !page.hostname.startsWith('[') ? `[${page.hostname}]` : page.hostname;
    return `ws://${host || '127.0.0.1'}:${port}${path}`;
}

export function scanRequest(interfaceName, cidr, timeoutMs = 3000) {
    const body = { timeout_ms: Number(timeoutMs) };
    if (!Number.isInteger(body.timeout_ms) || body.timeout_ms < 500 || body.timeout_ms > 10000)
        throw new Error('扫描等待时间应为 500–10000 毫秒');
    if (interfaceName) body.interface = interfaceName;
    if (cidr.trim()) {
        if (!interfaceName) throw new Error('指定扫描范围前请选择网卡');
        body.cidr = cidr.trim();
    }
    return body;
}
