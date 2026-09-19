#include <iostream>
#include <string>

int main() {
    std::cout << "ADV-START" << std::endl;
    std::string* p = nullptr;
    std::cout << "OBS=LEN:" << p->size() << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
