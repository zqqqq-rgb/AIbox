# business/ —— businessd 业务进程（M4 任务卡）
# 依赖：sqlite3（T4.1）、cpp-httplib（T4.4 REST/静态文件；WS 独立实现于 ws_server.*）、
#       jsoncpp（经 aibox_core 传递）
# 目标平台 Linux 板端/CI；探测模式与 engine/ 的 jsoncpp 一致
#
# CMake 兼容性纪律（板上实测踩中）：if(<变量名>) 在变量未定义时按字符串常量
# 求值（非 false 常量=真）——老 CMake 无 FindSQLite3 模块时 SQLite3_FOUND 未定义，
# if(SQLite3_FOUND) 恒真 → 误链接不存在的 imported target。
# 本文件判定一律：先 set 显式布尔（HAVE_*），再 if(<布尔变量>)。

# ---- sqlite3 ----
set(HAVE_SQLITE3_TARGET FALSE)
set(HAVE_SQLITE3_PC FALSE)
set(HAVE_SQLITE3_RAW FALSE)
set(SQLITE3_RAW_INC_DIRS "")
set(SQLITE3_RAW_LIBS "")
if(TARGET SQLite3::SQLite3)  # CMake >= 3.14 FindSQLite3 提供 imported target
  set(HAVE_SQLITE3_TARGET TRUE)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 QUIET sqlite3)
    if(DEFINED SQLITE3_LIBRARIES)
      set(HAVE_SQLITE3_PC TRUE)
    endif()
  endif()
endif()
if(NOT HAVE_SQLITE3_TARGET AND NOT HAVE_SQLITE3_PC)
  # 第三层兜底：直接 find_path/find_library（板上可能无 pkg-config 但有 dev 包）
  find_path(SQLITE3_INCLUDE_DIR sqlite3.h PATHS /usr/include /usr/local/include)
  find_library(SQLITE3_LIBRARY NAMES sqlite3 PATHS /usr/lib /usr/local/lib)
  if(SQLITE3_INCLUDE_DIR AND SQLITE3_LIBRARY)
    set(SQLITE3_RAW_INC_DIRS ${SQLITE3_INCLUDE_DIR})
    set(SQLITE3_RAW_LIBS ${SQLITE3_LIBRARY})
    set(HAVE_SQLITE3_RAW TRUE)
  endif()
endif()

# ---- cpp-httplib（header-only 或发行版共享库两种形态；探测后必须把库链上——
# Ubuntu 24 的 libcpp-httplib-dev 头文件仅含声明，实现任 libcpp-httplib.so）----
set(HAVE_HTTPLIB_TARGET FALSE)
set(HAVE_HTTPLIB_PC FALSE)
set(HTTPLIB_INC_DIRS "")
set(HTTPLIB_LIBS "")
find_package(cpp_httplib QUIET CONFIG)
if(TARGET cpp_httplib::httplib)
  set(HAVE_HTTPLIB_TARGET TRUE)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(CPP_HTTPLIB QUIET cpp-httplib)
    if(DEFINED CPP_HTTPLIB_LIBRARIES)
      set(HAVE_HTTPLIB_PC TRUE)
      set(HTTPLIB_INC_DIRS ${CPP_HTTPLIB_INCLUDE_DIRS})
      set(HTTPLIB_LIBS ${CPP_HTTPLIB_LIBRARIES})
    endif()
  endif()
endif()
if(NOT HAVE_HTTPLIB_TARGET AND NOT HAVE_HTTPLIB_PC)
  find_path(HTTPLIB_INCLUDE_DIR httplib.h PATHS /usr/include /usr/local/include)
  find_library(CPP_HTTPLIB_LIB NAMES cpp-httplib PATHS /usr/lib /usr/local/lib)
  if(HTTPLIB_INCLUDE_DIR)
    set(HTTPLIB_INC_DIRS ${HTTPLIB_INCLUDE_DIR})
    if(CPP_HTTPLIB_LIB)
      set(HTTPLIB_LIBS ${CPP_HTTPLIB_LIB})
    endif()
  else()
    message(FATAL_ERROR "未找到 cpp-httplib（Ubuntu: apt install libcpp-httplib-dev）")
  endif()
endif()

# ---- aibox_business：业务进程逻辑（纯逻辑，x86 CI 可编译可测）----
add_library(aibox_business STATIC
  db.cpp
  config_compiler.cpp
  uds_client.cpp
  zlm_client.cpp
  http_api.cpp
  ws_server.cpp
  hook_server.cpp
  janitor.cpp
  onvif_client.cpp
)
target_include_directories(aibox_business PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${HTTPLIB_INC_DIRS}
)
if(HAVE_HTTPLIB_TARGET)
  target_link_libraries(aibox_business PUBLIC cpp_httplib::httplib)
elseif(HTTPLIB_LIBS)
  # PUBLIC：测试可执行文件（test_ws_hub/test_http_api 直用 httplib::Client）传递链接
  target_link_libraries(aibox_business PUBLIC ${HTTPLIB_LIBS})
endif()

if(HAVE_SQLITE3_TARGET)
  target_link_libraries(aibox_business PUBLIC SQLite3::SQLite3)
elseif(HAVE_SQLITE3_PC)
  target_include_directories(aibox_business PUBLIC ${SQLITE3_INCLUDE_DIRS})
  target_link_libraries(aibox_business PUBLIC ${SQLITE3_LIBRARIES})
elseif(HAVE_SQLITE3_RAW)
  target_include_directories(aibox_business PUBLIC ${SQLITE3_RAW_INC_DIRS})
  target_link_libraries(aibox_business PUBLIC ${SQLITE3_RAW_LIBS})
else()
  message(FATAL_ERROR "未找到 sqlite3，请安装（Ubuntu: apt install libsqlite3-dev）")
endif()
target_link_libraries(aibox_business PUBLIC aibox_core)

add_executable(businessd main.cpp)
target_link_libraries(businessd PRIVATE aibox_business pthread)
