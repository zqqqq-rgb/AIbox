#include "infer/NpuCorePlan.h"

#include <fstream>
#include <set>

namespace aibox {

bool NpuCorePlan::valid(std::string* err) const {
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };
    if (steady_cores.empty()) return fail("steady 队列未分配核");
    if (triggered_cores.empty()) return fail("triggered 队列未分配核");
    std::set<uint8_t> seen;
    for (int q = 0; q < 2; ++q) {
        const auto& v = q == 0 ? steady_cores : triggered_cores;
        for (uint8_t c : v) {
            if (c > 2) return fail("核 id 越界（0~2）");
            if (!seen.insert(c).second) return fail("核被重复分配");
        }
    }
    return true;
}

bool NpuCorePlan::default_for(size_t total, NpuCorePlan& out, std::string* err) {
    if (total == 2) {
        out = {{0}, {1}};
        return true;
    }
    if (total == 3) {
        // 3588 默认：steady 2 核（吞吐×2 + 长尾池化），triggered 1 核 + pri 逐出兜底
        out = {{0, 1}, {2}};
        return true;
    }
    if (err) *err = "不支持的核数（仅 2/3）";
    return false;
}

NpuCorePlan NpuCorePlan::auto_detect(std::string* note) {
    // /proc/device-tree/compatible：NUL 分隔兼容串（如 "rockchip,rk3588-xxx"…），
    // 文本包含 SoC 名即命中（开发机无此文件 → 读空 → 走未知兜底）
    std::ifstream f("/proc/device-tree/compatible", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    size_t total = 0;
    if (text.find("rk3588") != std::string::npos) {
        total = 3;
    } else if (text.find("rk3576") != std::string::npos) {
        total = 2;
    }
    if (total == 0) {
        if (note) *note = "SoC 未知（非 rk3588/rk3576），按 2 核兜底";
        total = 2;
    } else if (note) {
        *note = "device-tree 检测 " + std::to_string(total) + " 核";
    }
    NpuCorePlan plan;
    default_for(total, plan);
    return plan;
}

bool NpuCorePlan::parse(const std::string& s, NpuCorePlan& out, std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    auto trim = [](const std::string& v) {
        const size_t b = v.find_first_not_of(" \t");
        if (b == std::string::npos) return std::string();
        const size_t e = v.find_last_not_of(" \t");
        return v.substr(b, e - b + 1);
    };
    NpuCorePlan p;
    bool has_steady = false, has_trig = false;
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t eq = s.find('=', pos);
        const size_t nx = s.find(':', pos);
        if (eq == std::string::npos || (nx != std::string::npos && eq > nx))
            return fail("格式须为 steady=<ids>:trig=<ids>");
        const std::string key = trim(s.substr(pos, eq - pos));
        const std::string ids =
            s.substr(eq + 1, nx == std::string::npos ? std::string::npos : nx - (eq + 1));
        std::vector<uint8_t> v;
        size_t b = 0;
        while (b <= ids.size()) {
            const size_t comma = ids.find(',', b);
            const std::string tok =
                trim(ids.substr(b, comma == std::string::npos ? std::string::npos : comma - b));
            if (!tok.empty()) {
                if (tok.size() != 1 || tok[0] < '0' || tok[0] > '2')
                    return fail("核 id 须为 0/1/2: " + tok);
                v.push_back(static_cast<uint8_t>(tok[0] - '0'));
            }
            if (comma == std::string::npos) break;
            b = comma + 1;
        }
        if (v.empty()) return fail("队列核列表为空: " + key);
        if (key == "steady" || key == "st") {
            p.steady_cores = v;
            has_steady = true;
        } else if (key == "triggered" || key == "trig") {
            p.triggered_cores = v;
            has_trig = true;
        } else {
            return fail("未知队列名: " + key + "（steady|triggered）");
        }
        if (nx == std::string::npos) break;
        pos = nx + 1;
    }
    if (!has_steady || !has_trig) return fail("须同时给出 steady 与 trig");
    if (!p.valid(err)) return false;
    out = p;
    return true;
}

}  // namespace aibox
