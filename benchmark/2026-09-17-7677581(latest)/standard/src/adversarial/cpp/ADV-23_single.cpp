#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
    std::cout << "ADV-START" << std::endl;
    std::ifstream in("inputs/ADV-23.bin");
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::cout << "OBS=CP:" << s.size() << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
