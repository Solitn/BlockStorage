#include "config.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <string>

#include "files.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace {

const char* kExtractDirName = "extract";

}  // namespace

// 默认导出根 = 程序工作区（当前工作目录）下的 extract/
std::string DefaultOutputRoot() {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) return std::string(kExtractDirName);
    cwd /= kExtractDirName;
    return PathToUtf8(cwd);
}

std::string EnvOutputRoot() {
#ifdef _WIN32
    // 环境变量可能是非 ASCII，必须走宽字符 API
    DWORD need = GetEnvironmentVariableW(L"BLOCKSTORAGE_OUT", nullptr, 0);
    if (need == 0) return {};
    std::wstring buf(need, L'\0');
    DWORD got = GetEnvironmentVariableW(L"BLOCKSTORAGE_OUT", buf.data(), need);
    if (got == 0 || got >= need) return {};
    buf.resize(got);
    return WideToUtf8(buf);
#else
    const char* env = std::getenv("BLOCKSTORAGE_OUT");
    return (env && env[0]) ? std::string(env) : std::string();
#endif
}

std::string OutputRoot() {
    const std::string from_env = EnvOutputRoot();
    if (!from_env.empty()) return from_env;

    // config 与索引合并：导出根存在加密的 idx 里
    const std::string from_index = IndexOutputRoot();
    if (!from_index.empty()) return from_index;

    return DefaultOutputRoot();
}

bool EnsureDirectory(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    const fs::path dir = PathFromUtf8(path);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) {
        err = ec.message();
        return false;
    }
    return true;
}

bool SetOutputRoot(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    if (!EnsureDirectory(path, err)) return false;
    return SetIndexOutputRoot(path);
}
