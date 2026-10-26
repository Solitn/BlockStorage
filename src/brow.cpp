#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "command.h"
#include "config.h"
#include "files.h"
#include "main.h"
#include "store.h"
#include "types.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace {

std::string g_repo = "/";
std::string g_local;

void Show(const Result& r) {
    if (!r.status) {
        std::cout << "Error: " << r.message << std::endl;
    } else if (!r.message.empty()) {
        std::cout << r.message << std::endl;
    }
}

std::string ParentOf(const std::string& path) {
    std::vector<std::string> segs = SplitInternalPath(path);
    if (segs.size() <= 1) return "/";
    segs.pop_back();
    std::string out;
    for (const std::string& s : segs) out += "/" + s;
    return out;
}

fs::path LocalPath(const std::string& raw) {
    const fs::path p = PathFromUtf8(raw);
    if (p.is_absolute()) return p;
    return PathFromUtf8(g_local) / p;
}

void PrintHelp() {
    std::cout << "  ls [-r]            list the current repository folder\n"
              << "  cd <path>          enter a folder (absolute or relative)\n"
              << "  up                 go to the parent folder\n"
              << "  pwd                print the current repository folder\n"
              << "  lls [path]         list a local folder\n"
              << "  lcd <path>         change the local folder\n"
              << "  lpwd               print the local folder\n"
              << "  add [path] [name]  quick add from the local folder\n"
              << "  get [path] [dir]   export (keeps the repository structure)\n"
              << "  rm <name> [-r]     remove an entry\n"
              << "  help | quit        this help / leave the browser" << std::endl;
}

void ListRepo(bool recursive) {
    Args a{"ls", g_repo};
    if (recursive) a.push_back("-r");
    Show(cmd_ls(a));
}

void ListLocal(const std::string& raw_dir) {
    const fs::path dir = raw_dir.empty() ? PathFromUtf8(g_local) : LocalPath(raw_dir);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cout << "Error: not a local folder: " << PathToUtf8(dir) << std::endl;
        return;
    }
    struct Row {
        std::string name;
        std::uint64_t size;
        bool dir;
    };
    std::vector<Row> rows;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code type_ec;
        const bool is_dir = it->is_directory(type_ec);
        std::uint64_t size = 0;
        if (!is_dir) store::file_size_of(it->path(), size);
        rows.push_back({PathToUtf8(it->path().filename()), size, is_dir});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.dir != b.dir) return a.dir;
        return a.name < b.name;
    });
    std::cout << "TYPE  SIZE          NAME\n";
    for (const Row& r : rows) {
        if (r.dir) {
            std::cout << "dir   -             " << r.name << "/\n";
            continue;
        }
        const std::string size = std::to_string(r.size);
        std::cout << "file  " << size;
        if (size.size() < 13) std::cout << std::string(13 - size.size(), ' ');
        std::cout << " " << r.name << "\n";
    }
}

}  // namespace

Result cmd_brow(const Args& args) {
    Result pre = require_loaded();
    if (!pre.status) return pre;

    g_repo = "/";
    if (args.size() >= 2) {
        const std::string target = NormalizeInternalPath(args[1]);
        if (target.empty()) return {false, "Invalid internal path"};
        std::string kind;
        if (!node_kind(target, kind)) {
            return {false, "No such file or directory in repository: " + args[1]};
        }
        if (kind != "dir") return {false, "Not a folder: " + args[1]};
        g_repo = target;
    }

    std::error_code ec;
    g_local = PathToUtf8(fs::current_path(ec));
    if (ec) g_local = ".";

    std::cout << "BlockStorage browser. 'help' for commands, 'quit' to leave.\n"
              << "repo  : " << g_repo << "\n"
              << "local : " << g_local << std::endl;

    std::string line;
    while (true) {
        std::cout << "brow:" << g_repo << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }
        line = NormalizeInputEncoding(line);
        const Args parts = SplitCommandLine(line);
        if (parts.empty()) {
            ListRepo(false);
            continue;
        }
        const std::string& c = parts[0];

        if (c == "quit" || c == "exit" || c == "q") break;
        if (c == "help" || c == "?") {
            PrintHelp();
            continue;
        }
        if (c == "pwd") {
            std::cout << g_repo << "\n";
            continue;
        }
        if (c == "lpwd") {
            std::cout << g_local << "\n";
            continue;
        }
        if (c == "ls") {
            ListRepo(parts.size() > 1 && (parts[1] == "-r" || parts[1] == "--recursive"));
            continue;
        }
        if (c == "up") {
            g_repo = ParentOf(g_repo);
            std::cout << g_repo << "\n";
            continue;
        }
        if (c == "cd") {
            if (parts.size() < 2) {
                std::cout << "Usage: cd <path>\n";
                continue;
            }
            const std::string target = (parts[1] == "..") ? ParentOf(g_repo)
                                                          : JoinInternalPath(g_repo, parts[1]);
            std::string kind;
            if (target.empty() || !node_kind(target, kind)) {
                std::cout << "Error: no such folder: " << parts[1] << "\n";
                continue;
            }
            if (kind != "dir") {
                std::cout << "Error: not a folder: " << parts[1] << "\n";
                continue;
            }
            g_repo = target;
            continue;
        }
        if (c == "lls") {
            ListLocal(parts.size() >= 2 ? parts[1] : std::string());
            continue;
        }
        if (c == "lcd") {
            if (parts.size() < 2) {
                std::cout << "Usage: lcd <path>\n";
                continue;
            }
            const fs::path dir = LocalPath(parts[1]);
            std::error_code dir_ec;
            if (!fs::is_directory(dir, dir_ec)) {
                std::cout << "Error: not a local folder: " << parts[1] << "\n";
                continue;
            }
            const fs::path abs = fs::absolute(dir, dir_ec);
            g_local = PathToUtf8(dir_ec ? dir : abs);
            continue;
        }
        if (c == "add") {
            if (parts.size() < 2) {
                std::cout << "Usage: add <local path> [name]\n";
                continue;
            }
            const fs::path src = LocalPath(parts[1]);
            std::error_code src_ec;
            if (!fs::exists(src, src_ec)) {
                std::cout << "Error: no such local path: " << parts[1] << "\n";
                continue;
            }
            const std::string name =
                (parts.size() >= 3) ? parts[2] : PathToUtf8(src.filename());
            Args a{"add", PathToUtf8(src), JoinInternalPath(g_repo, name)};
            Show(cmd_add(a));
            continue;
        }
        if (c == "get") {
            const std::string path = (parts.size() >= 2) ? JoinInternalPath(g_repo, parts[1]) : g_repo;
            const std::string dst =
                (parts.size() >= 3) ? PathToUtf8(LocalPath(parts[2])) : g_local;
            std::string dir_err;
            if (!EnsureDirectory(dst, dir_err)) {
                std::cout << "Error: cannot create " << dst << ": " << dir_err << "\n";
                continue;
            }
            Show(get_file(path, dst, res_statu.key));
            continue;
        }
        if (c == "rm") {
            if (parts.size() < 2) {
                std::cout << "Usage: rm <name> [-r]\n";
                continue;
            }
            Args a{"rm", JoinInternalPath(g_repo, parts[1])};
            for (std::size_t i = 2; i < parts.size(); ++i) {
                if (parts[i] == "-r" || parts[i] == "--recursive") a.push_back("-r");
            }
            Show(cmd_rm(a));
            continue;
        }
        std::cout << "Unknown browser command: " << c << " (try 'help')\n";
    }
    return {true, ""};
}
