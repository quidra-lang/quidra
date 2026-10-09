#include "llvm_backend/type_lowering.hpp"

#include <bit>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace quidra::llvm_backend {

std::string float_literal(double value, const Type& type) {
    const double materialized =
        type.kind == TypeKind::Real32
            ? static_cast<double>(static_cast<float>(value))
            : value;
    const auto bits = std::bit_cast<std::uint64_t>(materialized);
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase
        << std::setw(16) << std::setfill('0') << bits;
    return out.str();
}

} // namespace quidra::llvm_backend
