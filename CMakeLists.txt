cmake_minimum_required(VERSION 3.16)

# AIBox —— RK3588 智能视频分析盒（独立式三进程架构）
# 任务分解 T0.1：三目标 CMake 骨架 + 本地构建脚本 + format/tidy 规则
#
# 编译目标分层（任务分解 §1.2）：
#   aibox_core   纯逻辑（letterbox数学/调度/令牌桶/规则/协议/状态机），零硬件头文件
#   aibox_hw     MPP/RGA/RKNN 封装，实现 engine/interfaces/ 的硬件接口（仅板上编译）
#   core-engine  core + hw 装配（板端可执行）
#   businessd    业务进程（REST/WS/SQLite/MQTT）
#
# 默认（x86 开发机/CI）只编译 aibox_core 与 businessd（纯逻辑可编译）；
# 板上编译加 -DAIBOX_BUILD_HW=ON -DAIBOX_BUILD_ENGINE=ON。

project(aibox VERSION 0.1.0 LANGUAGES CXX)

# 可执行产物统一落 build/ 根（CMake 默认按源码树镜像，子目录目标会散在 build/engine/ 等）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# ---- 构建开关 ----
option(AIBOX_BUILD_HW     "编译 aibox_hw 硬件层（需 MPP/RGA/RKNN 交叉工具链）" OFF)
option(AIBOX_BUILD_ENGINE "编译 core-engine 可执行（依赖 aibox_hw）"           OFF)
option(AIBOX_BUILD_TESTS  "编译单元测试"                                        OFF)

add_subdirectory(engine)
add_subdirectory(business)

if(AIBOX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
