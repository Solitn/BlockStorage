#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "json.h"
#include "main.h"
#include "store.h"

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
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "BLOCKSTORAGE_OUT") == 0 && buf != nullptr) {
        std::string value(buf);
        free(buf);
        return value;
    }
    return {};
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
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec && !fs::is_directory(path)) {
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
