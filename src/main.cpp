#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "command.h"
#include "main.h"
#include "types.h"
#include "utf8.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#endif

Status res_statu;
json files;

namespace {

int run(const Args& args) {
    cmd_prepare(args);

    std::string input;
    while (true) {
        std::cout << "BlockStorage> " << std::flush;
        if (!std::getline(std::cin, input)) {
            std::cout << std::endl;
            break;  // EOF (Ctrl+Z / redirected input)
        }
        if (input == "exit" || input == "quit") {
            break;
        }
        // 管道输入可能是 GBK（cmd）或 UTF-8（PowerShell/重定向），统一归一化
        input = NormalizeInputEncoding(input);
        if (input.empty()) {
            continue;
        }
        Result re = command(input);
        if (!re.status) {
            std::cout << "Error: " << re.message << std::endl;
        } else if (!re.message.empty()) {
            std::cout << re.message << std::endl;
        }
    }
    return 0;
}

}  // namespace

#ifdef _WIN32
// 宽字符入口：命令行参数以 UTF-16 到达，统一转 UTF-8 后再处理，避免 ANSI 代码页问题
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    Args args;
    if (argc > 1) args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        args.push_back(WideToUtf8(argv[i]));
    }
    return run(args);
}
#else
int main(int argc, char* argv[]) {
    Args args(argv + 1, argv + argc);
    return run(args);
}
#endif
