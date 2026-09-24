#include <cstdio>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string l1, l2;
    std::getline(std::cin, l1);
    std::getline(std::cin, l2);
    double a = std::stod(l1);
    double b = std::stod(l2);
    double m = a / b;
    double seq[3] = {3.0, m, 1.0};
    double best = seq[0];
    for (int i = 1; i < 3; ++i) {
        if (seq[i] > best) best = seq[i];
    }
    bool selfeq = (m == m);
    printf("OBS=MAX:%.6f|SELFEQ:%s\n", best, selfeq ? "true" : "false"); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
