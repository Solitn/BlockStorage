#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <fstream>
#include "types.h"
#include <iterator>

inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()
    );
}

extern Status res_statu;

inline std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }

        if (!in_quotes && std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }

    if (!token.empty()) {
        tokens.push_back(token);
    }

    return tokens;
}