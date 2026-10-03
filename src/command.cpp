#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <fstream>
#include "command.h"
#include "types.h"
#include "files.h"
const std::string VER = "0.1";
const std::map<std::string, CommandEntry> COMMANDS = {
    {"init",   {cmd_init,   "Initialize a new repository,add -pxxxxxxx could set a password for the repository"}},
    {"help",   {cmd_help,   "Show this help message"}},
    {"delete", {cmd_delete, "Delete a file from the repository"}}
};

void cmd_prepare(const Args& args){
    std::cout << "BlockStorage v" << VER << std::endl;
    std::cout << "BlockStorage,made by @solitn" << std::endl<<"Github: https://github.com/solitn/blockstorage" << std::endl;
    if(check_dir()){
        if(fs::exists(".block/idx") && fs::exists(".block/key") && fs::exists(".block/data.blk") && fs::exists(".block/bitmap") && fs::exists(".block/bitmap.small")){
            std::cout << "Repository found" << std::endl;
        } else {
            std::cout << "Repository found, but some files are missing" << std::endl;
        }
    }else{
        std::cout << "Repository not found" << std::endl;
    }
}

Result cmd_delete(const Args& args) {
    if (check_dir()){
        try{
            fs::remove_all(".block");
        } catch (const fs::filesystem_error& e){
            return {false, e.what()};
        }
        return {true, "deleted successfully"};
    }else{
        return {false, "Repository not initialized"};
    }
}

Result cmd_help(const Args& args) {
    std::cout << "Available commands:" << std::endl;
    for (const auto& [name, entry] : COMMANDS) {
        std::cout << "  " << name << ": " << entry.desc << std::endl;
    }
    return {true, ""};
}

Result cmd_init(const Args& args) {
    if (check_dir()) {
        return {
            false,
            "Repository already initialized"
        };
    }

    try {
        if (args.size() > 1 && args[1].substr(0, 2) == "-p") {
            std::string password = args[1].substr(2);
            
        }
        fs::create_directory(".block");
        std::ofstream(".block/idx").close();
        std::ofstream(".block/key").close();
        std::ofstream(".block/data.blk").close();
        std::ofstream(".block/bitmap").close();
        std::ofstream(".block/bitmap.small").close();
        return {
            true,
            "Repository initialized"
        };
    } catch (const std::exception&) {
        return {
            false,
            "Failed to initialize repository"
        };
    }
}

Result command(const std::string& input) {
    Args args = split(input);
    if (args.empty()) {
        return {
            false,
            "No command given"
        };
    }
    auto it = COMMANDS.find(args[0]);
    if (it != COMMANDS.end()) {
        return it->second.fn(args);
    } else {
        return {
            false,
            "Unknown command: " + args[0]
        };
    }
}