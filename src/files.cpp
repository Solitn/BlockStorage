#include "files.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

bool check_dir() {
    fs::path dir = ".block";
    if (fs::exists(dir)) {
        return true;
    } else {
        return false;
    }
}