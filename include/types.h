#pragma once
#include <string>
#include <vector>

using Args = std::vector<std::string>;

struct Result {
    bool status;
    std::string message;
};

struct CommandEntry {
    Result (*fn)(const Args&);
    const char* desc;
};