#include <string>
#include <iostream>
#include <map>
#include <vector>
#include "command.h"
#include "types.h"
#include "files.h"

const std::map<std::string, CommandEntry> COMMANDS = {
    {"init",   {cmd_init,   "Initialize a new repository"}},
    {"help",   {cmd_help,   "Show this help message"}},
};
Result cmd_help(const Args& args) {
    std::cout << "Available commands:" << std::endl;
    for (const auto& [name, entry] : COMMANDS) {
        std::cout << "  " << name << ": " << entry.desc << std::endl;
    }
    return {true, ""};
}
Result cmd_init(const Args& args) {
    if(check_dir()){
        return {
            false,
            "Repository already initialized"
        };
    }else{
        std::filesystem::create_directory(".block");
        return {
            true,
            "Repository initialized"
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