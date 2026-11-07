#pragma once
#include "quidra/ast.hpp"
#include <string>
#include <unordered_map>

namespace quidra {
// Canonical expression identity shared by source contracts and checked
// shaped function types. Names are normalized by the owning shape unit.
std::string range_bound_identity(
    const Expr& expression, const std::unordered_map<std::string, std::string>& names);
}
