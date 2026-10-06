#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <fstream>
#include "command.h"
#include "types.h"
#include "main.h"
#include "files.h"
#include "encrypt.h"


const std::string VER = "0.1";
const std::map<std::string, CommandEntry> COMMANDS = {
    {"init",   {cmd_init,   "Initialize a new repository,add -p<password> could set a password for the repository"}},
    {"help",   {cmd_help,   "Show this help message"}},
    {"delete", {cmd_delete, "Delete the repository"}},
    {"load",   {cmd_load,   "Load the repository,add -p<password> could unlock the repository"}},
    {"statu",  {cmd_statu,  "Show the current status of the repository"}}
};

Result getStatu(const Args& args) {
    if (fs::is_directory(".block")) {
        res_statu.isfind = true;
        if (fs::exists(".block/idx") && fs::exists(".block/key") && fs::exists(".block/data.blk") && fs::exists(".block/bitmap") && fs::exists(".block/bitmap.small")) {
            res_statu.iscomplete = true;
        } else {
            res_statu.iscomplete = false;
        }
        try{
            U8list files = read_file(".block/key");
            if (files.empty()) {
                res_statu.key_sha256.clear();
                res_statu.haskey = false;
                res_statu.salt.clear();
                res_statu.hasload = true;
            } else {
                res_statu.haskey = true;
                res_statu.key_sha256.assign(files.begin() + 32, files.end());
                res_statu.salt.assign(files.begin(), files.begin() + 32);;
            }
        } catch (std::exception& e) {
            res_statu.haskey = false;
            res_statu.hasload = false;
        }
    }
    else {
        res_statu={
            false,
            false,
            false,
            false,
            {},
            {},
            {}
        };

    }
    return {true, ""};
}


void cmd_prepare(const Args& args){
    res_statu={
        false,
        false,
        false,
        false,
        {},//key_sha256
        {},//key
        {}
    };
    std::cout << "BlockStorage v" << VER << std::endl;
    std::cout << "BlockStorage,made by @solitn" << std::endl<<"Github: https://github.com/solitn/blockstorage" << std::endl;
    getStatu({});
    if (res_statu.isfind) {
        if (res_statu.iscomplete) {
            std::cout << "Repository found" << std::endl;
        }else {
            std::cout << "Repository found, but some files are missing" << std::endl;
        }
    }else {
        std::cout << "Repository not found" << std::endl;
    }
}

Result command(const std::string& input) {
    Args args = SplitCommandLine(input);
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

Result cmd_statu(const Args& args){
    getStatu({});
    std::cout << "isfind     : " << (res_statu.isfind     ? "true" : "false") << "\n";
    std::cout << "iscomplete : " << (res_statu.iscomplete ? "true" : "false") << "\n";
    std::cout << "haskey     : " << (res_statu.haskey     ? "true" : "false") << "\n";
    std::cout << "hasload    : " << (res_statu.hasload    ? "true" : "false") << "\n";
    if (!res_statu.key_sha256.empty()) {
        std::cout << "key (sha256)  : " << Base64Encode(res_statu.key_sha256) << "\n";
        std::cout << "salt : " << Base64Encode(res_statu.salt) << "\n";
        std::cout << "key   : " << Base64Encode(res_statu.key) << "\n";
    }
    return {true, ""};
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

Result cmd_load(const Args& args) {
    getStatu({});
    if (!res_statu.iscomplete) {
        return {false, "Cannot load repository: repository is incomplete or missing"};
    }
    if (!res_statu.haskey){
        return {false, "Cannot load repository: no key provided"};
    }
    if (res_statu.hasload){
        return {false, "Repository already loaded"};
    }
    if (args[1].substr(0, 2) == "-p"){
        std::string input_password = args[1].substr(2);
        U8list u8_password = U8list(input_password.begin(), input_password.end());
        U8list u8_salt = res_statu.salt;
        U8list u8_pass_sha256 = res_statu.key_sha256;
        U8list u8_key = u8_password;
        u8_key.insert(u8_key.end(), u8_salt.begin(), u8_salt.end());
        U8list u8_final = SlowHash(u8_key);
        if (Sha256(u8_final) != u8_pass_sha256){
            return {false, "Incorrect password"};
        }else{
            res_statu.hasload = true;
            res_statu.key = u8_final;
            return {true, "Loaded repository"};
        }
    }else{
        return {false, "Incorrect arguments"};
    }
}

Result cmd_init(const Args& args) {
    /*
    password----->slowhash----->sha256----->final key
            salt
    salt+final key----->file
    */
    getStatu({});
    std::string input_password = "";
    std::string salt = "";
    U8list u8_pass_bytes = {};
    U8list u8_salt_bytes = {};
    if(res_statu.isfind){
        return {false, "Repository already exists"};
    }else{
        try{
            fs::create_directory(".block");
            const char* files[] = {
                ".block/idx",
                ".block/key",
                ".block/data.blk",
                ".block/bitmap",
                ".block/bitmap.small"
            };
            for (const char* f : files) {
                if (!fs::exists(f)) {
                    std::ofstream ofs(f, std::ios::binary);  // 创建空文件
                    ofs.close();
                }
            }
            for (auto s:args){
                if (s.substr(0, 2) == "-p") {
                    input_password = s.substr(2);
                    u8_pass_bytes = U8list(input_password.begin(), input_password.end());
                }
                if (s.substr(0, 2) == "-s") {
                    salt = s.substr(2);
                    u8_salt_bytes = U8list(salt.begin(), salt.end());
                    U8list a = u8_salt_bytes;
                    u8_salt_bytes = To32(a);
                }
            }
            if (input_password.empty() && salt.empty()) {
                return {true, "Repository initialized successfully"};
            }else{  //add key
                if (salt.empty()) {
                    std::cout << "No salt provided, generating random salt..." << std::endl;
                    u8_salt_bytes = RandomBytes(32);
                }
                U8list u8_final_bytes = u8_pass_bytes;
                u8_final_bytes.insert(u8_final_bytes.end(), u8_salt_bytes.begin(), u8_salt_bytes.end());//add salt to password
                U8list derived_key = SlowHash(u8_final_bytes);// slow hash pass+salt
                U8list sha256_key = Sha256(derived_key);   // sha256 slow hash pass+salt
                std::cout << "Derived key (SHA256): ";
                std::cout << Base64Encode(sha256_key) << std::endl;
                std::cout << "Final key: ";
                std::cout << Base64Encode(derived_key) << std::endl;
                std::cout << "Derived salt: ";
                std::cout << Base64Encode(u8_salt_bytes) << std::endl;
                std::ofstream key_file(".block/key", std::ios::binary);
                res_statu.key = derived_key;
                res_statu.hasload = true;
                U8list u8_keyfile_bytes = u8_salt_bytes;
                u8_keyfile_bytes.insert(u8_keyfile_bytes.end(), sha256_key.begin(), sha256_key.end());//add salt and sha256 to key file
                key_file.write(reinterpret_cast<const char*>(u8_keyfile_bytes.data()), u8_keyfile_bytes.size());
                if(!key_file.is_open()){
                    return {false, "Failed to write key file"};
                }
                key_file.close();
            }
            return {true, "Repository initialized successfully"};
        }catch (const fs::filesystem_error& e){
            return {false, e.what()};
        }
    }
}

std::vector<std::string> SplitCommandLine(const std::string& cmd) {
    std::vector<std::string> args;
    std::string cur;
    bool inQuotes = false;   // 是否处于引号内
    bool hasContent = false; // 当前参数是否已开始（用于识别空参数 ""）
    size_t i = 0, n = cmd.size();

    while (i < n) {
        char c = cmd[i];

        // 处理连续反斜杠
        if (c == '\\') {
            size_t start = i;
            while (i < n && cmd[i] == '\\') ++i;
            size_t cnt = i - start;

            if (i < n && cmd[i] == '"') {
                cur.append(cnt / 2, '\\');      // 每两个 \ 还原成一个
                if (cnt % 2 == 0) {
                    inQuotes = !inQuotes;       // 偶数个：引号起作用
                    hasContent = true;
                    ++i;
                } else {
                    cur.push_back('"');         // 奇数个：引号是字面量
                    hasContent = true;
                    ++i;
                }
            } else {
                cur.append(cnt, '\\');          // 后面不是引号，原样保留
                hasContent = true;
            }
            continue;
        }

        if (c == '"') {
            if (inQuotes && i + 1 < n && cmd[i + 1] == '"') {
                cur.push_back('"');             // 引号内的 "" -> 一个 "
                i += 2;
            } else {
                inQuotes = !inQuotes;           // 切换引号状态
                ++i;
            }
            hasContent = true;
            continue;
        }

        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (hasContent) {
                args.push_back(cur);
                cur.clear();
                hasContent = false;
            }
            ++i;
            continue;
        }

        cur.push_back(c);
        hasContent = true;
        ++i;
    }

    if (hasContent) args.push_back(cur);
    return args;
}

