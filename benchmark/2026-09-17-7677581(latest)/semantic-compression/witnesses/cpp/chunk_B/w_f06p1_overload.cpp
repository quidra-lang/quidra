#include <cstdio>
#include <vector>

int mid(const std::vector<int>& v);
int mid(std::vector<int>& v) { v.push_back(4); return 99; }

int caller() {
// BEGIN PROBE F06.P1
    std::vector<int> xs = {1, 2, 3};
    int y = mid(xs);
    return y;
}

int mid(const std::vector<int>& v) { return v[1]; }
// END PROBE F06.P1

int main() { std::printf("%d\n", caller()); }
