cmake_minimum_required(VERSION 3.16)

project(BlockStorage VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 便携工具链缺少 msvcprt.lib / vcruntime.lib 导入库，改用静态 CRT（/MT）
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

# OpenSSL 通过环境变量定位（scripts/env.ps1 设置）
if(DEFINED ENV{OPENSSL_ROOT_DIR})
    set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
    message(STATUS "OPENSSL_ROOT_DIR = ${OPENSSL_ROOT_DIR}")
endif()

# Windows 优先静态链接（配合 /MT，产物不依赖 libcrypto-*.dll）；
# 其它平台按系统默认方式查找 OpenSSL。
if(WIN32)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)

add_executable(BlockStorage
    src/main.cpp
    src/command.cpp
    src/brow.cpp
    src/files.cpp
    src/encrypt.cpp
    src/store.cpp
    src/config.cpp
    src/utf8.cpp
)

target_link_libraries(BlockStorage PRIVATE OpenSSL::Crypto)
target_include_directories(BlockStorage PRIVATE include)

if(MSVC)
    target_compile_options(BlockStorage PRIVATE /utf-8 /W3)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT BlockStorage)
else()
    target_compile_options(BlockStorage PRIVATE -Wall -Wextra)
endif()
