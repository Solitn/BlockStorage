cmake_minimum_required(VERSION 3.16)

project(BlockStorage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OPENSSL_ROOT_DIR    "C:/Program Files/OpenSSL-Win64")
set(OPENSSL_INCLUDE_DIR "C:/Program Files/OpenSSL-Win64/include")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_ossl_lib_dir "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MDd")
else()
    set(_ossl_lib_dir "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MD")
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(OPENSSL_CRYPTO_LIBRARY "${_ossl_lib_dir}/libcrypto.lib")
set(OPENSSL_SSL_LIBRARY    "${_ossl_lib_dir}/libssl.lib")

find_package(OpenSSL REQUIRED)
add_executable(BlockStorage 
    src/main.cpp
    src/command.cpp
    src/files.cpp
    src/encrypt.cpp
)
add_custom_command(TARGET BlockStorage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "C:/Program Files/OpenSSL-Win64/bin/libcrypto-4-x64.dll"
        "C:/Program Files/OpenSSL-Win64/bin/libssl-4-x64.dll"
        "$<TARGET_FILE_DIR:BlockStorage>"
)
find_package(OpenSSL REQUIRED)
target_link_libraries(BlockStorage PRIVATE OpenSSL::Crypto)
target_include_directories(BlockStorage PRIVATE include)