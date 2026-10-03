#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <fstream>
#include "command.h"
#include "types.h"
#include "main.h"
#include "encrypt.h"
#include "files.h"
const std::string VER = "0.1";
const std::map<std::string, CommandEntry> COMMANDS = {
    {"init",   {cmd_init,   "Initialize a new repository,add -p<password> could set a password for the repository"}},
    {"help",   {cmd_help,   "Show this help message"}},
    {"delete", {cmd_delete, "Delete a file from the repository"}},
    {"load",   {cmd_load,   "Load the repository,add -p<password> could unlock the repository"}},
    {"statu",  {cmd_statu,  "Show the current status of the repository"}}
};

void cmd_prepare(const Args& args){
    res_statu={
        false,
        false,
        false,
        false,
        {}
    };
    std::cout << "BlockStorage v" << VER << std::endl;
    std::cout << "BlockStorage,made by @solitn" << std::endl<<"Github: https://github.com/solitn/blockstorage" << std::endl;
    if(check_dir()){
        if(fs::exists(".block/idx") && fs::exists(".block/key") && fs::exists(".block/data.blk") && fs::exists(".block/bitmap") && fs::exists(".block/bitmap.small")){
            std::cout << "Repository found" << std::endl;
            res_statu.iscomplete = true;
        } else {
            std::cout << "Repository found, but some files are missing" << std::endl;
            res_statu.iscomplete = false;
        }
        res_statu.isfind = true;
        U8list files = read_file(".block/key");
        if(files.size() == 0){
            res_statu.key = {};
            res_statu.haskey = false;
            res_statu.hasload = true;
        } else {
            res_statu.haskey = true;
            res_statu.key = files;
        }
    }else{
        std::cout << "Repository not found" << std::endl;
    }
}
Result cmd_statu(const Args& args){
    std::cout << "isfind     : " << (res_statu.isfind     ? "true" : "false") << "\n";
    std::cout << "iscomplete : " << (res_statu.iscomplete ? "true" : "false") << "\n";
    std::cout << "haskey     : " << (res_statu.haskey     ? "true" : "false") << "\n";
    std::cout << "hasload    : " << (res_statu.hasload    ? "true" : "false") << "\n";
    std::cout << "key size   : " << res_statu.key.size() << "\n";
    if (!res_statu.key.empty()) {
        std::cout << "key (b64)  : " << encrypt::encode(res_statu.key) << "\n";
    }
    return {true, ""};
}
Result cmd_load(const Args& args){
    if (res_statu.iscomplete){
        if(!res_statu.haskey){
            return {true, "Repository loaded"};
        }else{
            if (args.size() > 1 && args[1].substr(0, 2) == "-p") {
                std::string inputpassword = args[1].substr(2);
                U8list pass;
                U8list data = read_file(".block/key");
                U8list salt(data.begin(), data.begin() + 32);
                U8list password = U8list(inputpassword.begin(), inputpassword.end());
                password.insert(password.begin(),salt.begin(),salt.end());
                U8list key = encrypt::encrypt_string(password,password);
                U8list shakey = encrypt::sha256(key);
                if (shakey == U8list(data.begin() + 32, data.end())){
                    return {true, "Repository loaded,the password is correct"};
                }else{
                    return {false, "Repository loaded,but the password is incorrect"};
                }
            }
        }
    }else{
        return {false, "Repository not complete"};
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
        res_statu = {false, false, false, false, {}};
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
    U8list salt(32);
    try {
        if (args.size() > 1 && args[1].substr(0, 2) == "-p") {
            std::string inputpassword = args[1].substr(2);
            res_statu.isfind = true;
            U8list pass;
            U8list password(inputpassword.begin(), inputpassword.end());
            password.insert(password.begin(),salt.begin(),salt.end());
            pass = encrypt::encrypt_string(U8list(password.begin(), password.end()), U8list(password.begin(), password.end()));
            res_statu.key = pass;
        }
        fs::create_directory(".block");
        std::ofstream(".block/idx").close();
        std::ofstream(".block/key").close();
        std::ofstream(".block/data.blk").close();
        std::ofstream(".block/bitmap").close();
        std::ofstream(".block/bitmap.small").close();
        std::ofstream io(".block/key", std::ios::binary | std::ios::app);
        if(res_statu.key.size() > 0) {
            for (auto& b : salt) b = encrypt::random_byte();
            io.write(reinterpret_cast<const char*>(salt.data()), salt.size());
            U8list shakey = encrypt::sha256(res_statu.key);
            io.write(reinterpret_cast<const char*>(shakey.data()), shakey.size());
        }
        if(!io) {
            return {
                false,
                "Failed to write to .block/key"
            };
        }
        io.close();
        res_statu.isfind = true;
        res_statu.iscomplete = true;
        if(res_statu.key.size() > 0) {
            std::string msg = "Repository initialized, password set successfully , key's base64 is ";
            res_statu.haskey = true;
            return {
                true,
                msg+=encrypt::encode(res_statu.key)
            };
        }else{
            res_statu.haskey = false;
            return {
                true,
                "Repository initialized, no password set"
            };
        }
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