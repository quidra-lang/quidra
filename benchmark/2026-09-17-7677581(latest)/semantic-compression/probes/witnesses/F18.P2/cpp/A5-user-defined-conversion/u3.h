#pragma once
#include <cstdint>
namespace util { struct R { std::int32_t v; operator std::int32_t() const; }; R pub_add(std::int32_t a, std::int32_t b); }
