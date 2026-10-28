#include "hw/NpuMetricsSource.h"

#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aibox {

std::string NpuMetricsSource::read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

MetricsSnapshot NpuMetricsSource::sample() {
    using namespace std::chrono;
    MetricsSnapshot s;
    s.ts_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

    // NPU 负载：/sys/kernel/debug/rknpu/load
    //   5.10 内核: "NPU load:  12%"；6.1 内核: "load0:xx% load1:xx% load2:xx%" 等变体。
    //   防御性解析：取文件中所有 "整数%"，最大者=三核峰值，前三个按序填 c0/c1/c2 语义
    {
        auto txt = read_file("/sys/kernel/debug/rknpu/load");
        float max_pct = 0;
        int idx = 0;
        for (size_t i = 0; i < txt.size();) {
            if (std::isdigit(static_cast<unsigned char>(txt[i]))) {
                size_t j = i;
                while (j < txt.size() && std::isdigit(static_cast<unsigned char>(txt[j]))) ++j;
                if (j + 1 < txt.size() && txt[j] == '%') {
                    float v = std::atof(txt.substr(i, j - i).c_str());
                    if (idx < 3) {
                        if (idx == 0) s.npu_load_pct = v;
                        (void)idx;  // c0/c1/c2 细分由 get_metrics 组装（M4）
                        ++idx;
                    }
                    max_pct = std::max(max_pct, v);
                    i = j + 1;
                    continue;
                }
                i = j;
                continue;
            }
            ++i;
        }
        s.npu_load_pct = max_pct;  // 降级③取三核峰值（详设 §3.9）
    }

    // DDR 带宽占用：/sys/class/devfreq/dmc/load（"xx%"）；无 DFI 驱动 → 0
    {
        auto txt = read_file("/sys/class/devfreq/dmc/load");
        auto pos = txt.find('%');
        if (pos != std::string::npos) {
            size_t b = pos;
            while (b > 0 && (std::isdigit(static_cast<unsigned char>(txt[b - 1]))))
                --b;
            s.ddr_pct = std::atof(txt.substr(b, pos - b).c_str());
        }
    }

    // 温度：/sys/class/thermal/thermal_zone*/temp（毫℃）取最大（SoC 单温区 ≈ NPU 温度）
    {
        DIR* d = opendir("/sys/class/thermal");
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (std::strncmp(de->d_name, "thermal_zone", 12) != 0) continue;
                auto txt = read_file(std::string("/sys/class/thermal/") + de->d_name +
                                     "/temp");
                if (txt.empty()) continue;
                int milli = std::atoi(txt.c_str());
                if (milli > 0 && milli < 150000)  // 合理性过滤
                    s.npu_temp_c = std::max(s.npu_temp_c, milli / 1000.f);
            }
            closedir(d);
        }
    }

    return s;
}

}  // namespace aibox
