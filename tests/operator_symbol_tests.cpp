#include "quidra/abi/symbols.hpp"

#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::vector<std::string> names{
        "$method.Values.operator()", "$method.Values.operator[]",
        "$method.Values.operator__", "$method.Values.operator",
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (quidra::abi::user_symbol(names[i]) == quidra::abi::user_symbol(names[j]) ||
                quidra::abi::generic_instance_prefix("gm", names[i]) ==
                    quidra::abi::generic_instance_prefix("gm", names[j])) {
                std::cerr << "operator symbol collision: " << names[i] << " and " << names[j] << '\n';
                return 1;
            }
            const auto first = quidra::abi::generic_instance_prefix("gm", names[i]) + "123";
            const auto second = quidra::abi::generic_instance_prefix("gm", names[j]) + "123";
            if (quidra::abi::user_symbol(first) == quidra::abi::user_symbol(second)) {
                std::cerr << "generic operator symbol collision\n";
                return 1;
            }
        }
    }
    std::cout << "operator symbol tests passed\n";
}
