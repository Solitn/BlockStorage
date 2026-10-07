#pragma once
#include <string>
#include <vector>
#include "json.h"
#include "types.h"
U8list To32(const U8list& in);
std::string Base64Encode(const std::vector<uint8_t>& data);
U8list SlowHash(const U8list& data);
U8list Sha256(const U8list& data);
U8list RandomBytes(size_t n);