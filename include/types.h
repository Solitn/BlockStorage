#pragma once
#include <string>
#include <vector>
#include <cstdint>

using Args = std::vector<std::string>;
using U8list = std::vector<uint8_t>;
struct Result {
    bool status;
    std::string message;
};

struct CommandEntry {
    Result (*fn)(const Args&);
    const char* desc;
};

struct Status {
    bool isfind;
    bool iscomplete;
    bool haskey;
    bool hasload;
    U8list key_sha256;
    U8list key;
    U8list salt;
};

