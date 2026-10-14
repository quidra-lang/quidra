#pragma once
#include "quidra/compiler_extension_model.hpp"
#include <string_view>
namespace quidra {
CompilerExpression parse_compiler_expression(std::string_view,
    CompilerExpression::Category, std::string_view origin);
} // namespace quidra
