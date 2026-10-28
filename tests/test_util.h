// 测试共用：极简断言宏（M1 六个单测零外部依赖，ctest 直跑）
#pragma once

#include <cstdio>

static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(((a) - (b)) < (eps) && ((b) - (a)) < (eps))

// 带上下文的失败输出：CHECK_MSG(cond, "fmt", args...)（fmt 为 fprintf 格式串）
#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            g_fail = 1;                                                      \
        }                                                                    \
    } while (0)

#define TEST_MAIN(name)                  \
    int main() {                         \
        run();                           \
        if (g_fail == 0) printf(name " PASSED\n"); \
        return g_fail;                   \
    }
