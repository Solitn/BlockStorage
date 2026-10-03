#include <iostream>
#include <string>
#include "command.h"
#include "main.h"

#define VER "0.1"

int main() {
    std::string input;
    std::cout << "BlockStorage v" << VER << std::endl;
    std::cout << "BlockStorage,made by @solitn" << std::endl<<"Github: https://github.com/solitn/blockstorage" << std::endl;
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