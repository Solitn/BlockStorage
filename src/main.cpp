#include <iostream>
#include <string>
#include "command.h"
#include "main.h"

#define VER "0.1"
struct Status {
    bool isfind;
    bool iscomplete;
    std::string key;
};



int main(int argc, char* argv[]) {
    std::string input;
    cmd_prepare({});
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