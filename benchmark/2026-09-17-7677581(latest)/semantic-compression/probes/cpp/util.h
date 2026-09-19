#pragma once
// BEGIN PROBE F14.P3
struct Shape {
    virtual double area() const = 0;
    virtual ~Shape() = default;
};
struct Circle : Shape {
    double r;
    double area() const override { return 3.141592653589793 * r * r; }
};
struct Rect : Shape {
    double w, h;
    double area() const override { return w * h; }
};
// END PROBE F14.P3

#include <cstdint>

namespace util {
std::int32_t pub_add(std::int32_t a, std::int32_t b);
}
