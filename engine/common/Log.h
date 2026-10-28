// Log —— 轻量日志前缀（任务分解 §10.1 的最小落地：WARN/ERROR 级别自愈事件）
// 每行自动带墙钟时间戳 + 模块标签；热路径日志仍禁（架构 §10.1 级别纪律），
// 本头文件只服务 stderr 的诊断输出，不引入任何锁/IO 依赖。
// 用法：LOGE("[mpp] open fail %d", ret);  展开为
//       fprintf(stderr, "[12:34:56.789][mpp] open fail %d\n", ret);
#pragma once

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>

namespace aibox::log {

// 墙钟 "MM-DD HH:MM:SS.mmm"（本地时区）；静态缓冲 + strftime，无分配无锁
// （glibc 的 localtime_r 需 POSIX 声明；MSVC 无此函数走 localtime + 手动锁不存在——
//   本工程目标平台为 Linux 板端/CI，MSVC 分支仅保编译通过）
inline const char* ts() {
    // 缓冲 32：gcc -Wformat-truncation 保守按 uint16_t 上界推算 %03u 可写 5 字节，
    // 19+5=24 恰触边界（板上 aarch64 gcc 11 实测）；扩容一劳永逸，thread_local 零成本
    static thread_local char buf[32];
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::time_t sec =
        static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    const unsigned ms = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &sec);
#else
    localtime_r(&sec, &tm);
#endif
    // ms 已由 %1000 约束到 0~999（窄化仅辅助可读性）
    const auto ms_u = static_cast<uint16_t>(ms);
    std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d:%02d.%03u",
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms_u);
    return buf;
}

}  // namespace aibox::log

// level: E=ERROR(需人工) W=自愈事件 I=状态变更（均非逐帧热路径）
#define LOGE(mod, ...) \
    std::fprintf(stderr, "[%s][E][%s] ", aibox::log::ts(), mod), \
        std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr)
#define LOGW(mod, ...) \
    std::fprintf(stderr, "[%s][W][%s] ", aibox::log::ts(), mod), \
        std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr)
#define LOGI(mod, ...) \
    std::fprintf(stderr, "[%s][I][%s] ", aibox::log::ts(), mod), \
        std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr)
