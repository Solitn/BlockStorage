#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "command.h"
#include "main.h"
#include "types.h"

#define VER "0.1"

Status res_statu;
json files;

int main(int argc, char* argv[]) {
    Args args(argv + 1, argv + argc);
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
