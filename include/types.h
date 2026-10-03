#pragma once
#include <string>
#include <vector>
#include <cstdint>

using Args = std::vector<std::string>;

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
    std::vector<uint8_t> key;
};

