#include <iostream>
std::ostream& operator<<(std::ostream& os, const char (&s)[3]) {
    return os.write("Y\n", 2);
}
int main() {
    std::cout << "x\n";
}
