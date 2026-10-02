cmake_minimum_required(VERSION 3.16)

project(BlockStorage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(blockstorage 
    src/main.cpp
    src/command.cpp
    src/files.cpp
)
target_include_directories(blockstorage PRIVATE include)