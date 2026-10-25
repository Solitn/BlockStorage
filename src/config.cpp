#include "config.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "json.h"
#include "main.h"
#include "store.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace {

const char* kDefaultRoot = "C:\\BlockStorage\\extract";

}  // namespace

std::string DefaultOutputRoot() {
    return kDefaultRoot;
}

std::string ConfigFilePath() {
    return store::layout().dir + "/config.json";
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

    const std::string cfg_path = ConfigFilePath();
    std::ifstream in(cfg_path, std::ios::binary);
    if (in) {
        json j = json::parse(in, nullptr, false);
        if (!j.is_null() && j.is_object() && j.contains("output_root") &&
            j["output_root"].is_string()) {
            std::string v = j["output_root"].get<std::string>();
            if (!v.empty()) return v;
        }
    }
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

    const std::string cfg_path = ConfigFilePath();
    json j = json::object();
    {
        std::ifstream in(cfg_path, std::ios::binary);
        if (in) {
            json old = json::parse(in, nullptr, false);
            if (old.is_object()) j = old;
        }
    }
    j["output_root"] = path;

    std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + cfg_path;
        return false;
    }
    out << j.dump(2);
    if (!out.good()) {
        err = "cannot write " + cfg_path;
        return false;
    }
    return true;
}
