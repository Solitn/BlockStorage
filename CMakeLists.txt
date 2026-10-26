cmake_minimum_required(VERSION 3.16)

project(BlockStorage VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 便携工具链缺少 msvcprt.lib / vcruntime.lib 导入库，改用静态 CRT（/MT）
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

# 加密走 Windows 自带 CNG（bcrypt）：静态 CRT + 系统 DLL，不依赖任何第三方运行时
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

target_link_libraries(BlockStorage PRIVATE bcrypt)
target_include_directories(BlockStorage PRIVATE include)

if(MSVC)
    target_compile_options(BlockStorage PRIVATE /utf-8 /W3)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT BlockStorage)
else()
    target_compile_options(BlockStorage PRIVATE -Wall -Wextra)
endif()
