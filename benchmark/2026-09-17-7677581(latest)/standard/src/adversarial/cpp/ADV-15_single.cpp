#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::cout << "ADV-START" << std::endl;
    std::vector<std::int64_t> v{1, 2, 3, 4, 5};
    std::int64_t iters = 0;
    for (std::int64_t e : v) {
        iters = iters + 1;
        if (e == 2) v.push_back(99);
    }
    std::cout << "OBS=ITERS:" << iters << "|LEN:" << v.size() << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
