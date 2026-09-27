cmake_minimum_required(VERSION 3.16)
project(NovaServer LANGUAGES CXX)

# ---- 语言标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 默认构建类型 ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---- 优化级别 ----
# 显式固定 Release 的优化级别。CMake 默认使用 -O3，而性能基线以 -O2 采集，
# 保持两者一致才能让后续的性能对比只反映代码改动本身。
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# ---- 可选项 ----
option(ENABLE_ASAN "启用 AddressSanitizer" OFF)
option(ENABLE_TSAN "启用 ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN 与 ENABLE_TSAN 不能同时启用")
endif()

# ---- 源文件 ----
set(NOVASERVER_SOURCES
    main.cpp
    webserver.cpp
    config.cpp
    http/http_conn.cpp
    auth/password_hash.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

add_executable(server ${NOVASERVER_SOURCES})

target_include_directories(server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---- 依赖 ----
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mysqlclient)

# OpenSSL 仅用于口令的 PBKDF2-HMAC-SHA256
find_package(OpenSSL REQUIRED)

target_link_libraries(server PRIVATE
    Threads::Threads
    PkgConfig::MYSQLCLIENT
    OpenSSL::Crypto
)

# ---- 警告 ----
target_compile_options(server PRIVATE -Wall -Wextra)

# ---- 动态检查工具 ----
if(ENABLE_ASAN)
    target_compile_options(server PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(server PRIVATE -fsanitize=address)
endif()

if(ENABLE_TSAN)
    target_compile_options(server PRIVATE -fsanitize=thread)
    target_link_options(server PRIVATE -fsanitize=thread)
endif()

# ---- 单元测试 ----
# 通过 -DBUILD_TESTING=OFF 可关闭，依赖 GoogleTest
include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_subdirectory(tests)
endif()
