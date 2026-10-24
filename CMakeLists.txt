cmake_minimum_required(VERSION 3.16)

project(BlockStorage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 便携工具链缺少 msvcprt.lib / vcruntime.lib 导入库，改用静态 CRT（/MT）
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

# OpenSSL 通过环境变量定位（scripts/env.ps1 会设置）；不再硬编码绝对路径
if(DEFINED ENV{OPENSSL_ROOT_DIR})
    set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
    message(STATUS "OPENSSL_ROOT_DIR = ${OPENSSL_ROOT_DIR}")
else()
    message(WARNING "OPENSSL_ROOT_DIR is not set; falling back to default search paths")
endif()

find_package(OpenSSL REQUIRED)

add_executable(BlockStorage
    src/main.cpp
    src/command.cpp
    src/files.cpp
    src/encrypt.cpp
    src/store.cpp
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

# 构建后把 OpenSSL 运行库复制到输出目录（存在才复制）
if(WIN32)
    file(GLOB _ossl_dlls "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll" "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll")
    if(_ossl_dlls)
        add_custom_command(TARGET BlockStorage POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_ossl_dlls}
                "$<TARGET_FILE_DIR:BlockStorage>"
        )
    endif()
endif()
