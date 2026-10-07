#include "files.h"
#include "types.h"
#include "main.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "json.h"

bool check_dir() {
    fs::path dir = ".block";
    if (fs::exists(dir)) {
        return true;
    } else {
        return false;
    }
}

Result add_file(std::string in_file_name /*内部名称*/ , std::string out_file_name ,U8list key) {
    bool haskey = false;
    if (key.size()==32){
        haskey = true;
    }else{
        key = U8list{0x5E, 0x1B, 0x9C, 0x47, 0xD2, 0x80, 0x3A, 0xF6,
            0x28, 0xB5, 0x6D, 0xE1, 0x03, 0x97, 0x4C, 0xAA,
            0x71, 0x0F, 0xC8, 0x34, 0x96, 0x5B, 0xE2, 0x19,
            0x8E, 0xD4, 0x27, 0x63, 0xFA, 0x08, 0xB1, 0x4D
        };
    }
    read_file(".block/file.json");
    return {true , ""};
}