#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include "command.h"
#include "main.h"
#include "types.h"

#define VER "0.1"

Status res_statu;
json files;
int main(int argc, char* argv[]) {
    std::string input;
    Args args(argv + 1, argv + argc);
    cmd_prepare(args);
    while (true) {
        std::cout << "BlockStorage> ";
        std::getline(std::cin, input);
        if (input == "exit") {
            break;
        }
        else{
            Result re = command(input);
            if (re.status == false) {
                std::cout << "Error: " << re.message << std::endl;
            }else{
                if (re.message != "") {
                    std::cout << re.message << std::endl;
                }
            }
        }
    }
    return 0;
}