// The exact-real provider of the exactprobe probe package: atom 1 is the
// square root of two (its decimal expansion, from which Core takes rational
// enclosures of any width it needs), and unary operation 1 is negation.
#include <quidra/native_extension.h>

#include <cmath>
#include <cstdint>

namespace {

const char* exactprobe_atom_decimal(std::uint32_t opcode) {
    switch (opcode) {
        case 1:
            return "1.41421356237309504880168872420969807856967187537694807317667973799073247846210703885038753432764157273501384623091229702492483605585073721264412149709993583141322266592750559275579995050115278206057147";
        default:
            return nullptr;
    }
}

int exactprobe_unary_evaluate(std::uint32_t opcode, double input, double* output) {
    if (!output || !std::isfinite(input)) return 0;
    switch (opcode) {
        case 1: *output = -input; return 1;
        default: return 0;
    }
}

std::uint32_t exactprobe_unary_flags(std::uint32_t opcode) {
    return opcode == 1 ? QCORE_EXACT_UNARY_TOTAL : 0;
}

struct ExactprobeProviderRegistration {
    ExactprobeProviderRegistration() {
        (void)qcore_exact_real_provider_register("exactprobe", exactprobe_atom_decimal);
        (void)qcore_exact_real_provider_register_unary(
            "exactprobe", exactprobe_unary_evaluate, exactprobe_unary_flags);
    }
};

const ExactprobeProviderRegistration exactprobe_provider_registration{};

} // namespace
