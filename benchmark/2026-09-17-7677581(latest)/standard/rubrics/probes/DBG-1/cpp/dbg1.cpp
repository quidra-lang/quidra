// DBG-1
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static const int64_t ITERATIONS = 300000000;
static const int64_t MODULUS = 1000000007;

struct Item {
    int64_t count;
    std::string name;
};

int64_t accumulate(const std::vector<Item>& items, int64_t iterations) {
    int64_t total = 0;
    for (int64_t i = 0; i < iterations; ++i) {
        for (const Item& item : items) {
            total = (total * 31 + item.count + (int64_t)item.name.size()) % MODULUS;
        }
    }
    return total;
}

int main() {
    std::vector<Item> items;
    items.push_back(Item{7, "alpha"});
    items.push_back(Item{11, "bravo"});
    items.push_back(Item{13, "charlie"});
    int64_t checksum = accumulate(items, ITERATIONS);
    printf("checksum=%lld\n", (long long)checksum);
    return 0;
}
