#include "ir/content_hash.hpp"
#include "platform/sha256.hpp"

namespace quidra::ir {
namespace {
Digest digest(std::string_view domain, std::string_view bytes) {
    platform::Sha256 hash;
    hash.update(domain);
    hash.update(bytes);
    return {hash.finish()};
}
} // namespace
FunctionIdentity identify(const Function& function) {
    const auto encoded = canonical_encode(function);
    return {digest("quidra-function-structural/1",encoded.bytes),
            digest("quidra-function-locations/1",canonical_location_bytes(encoded.locations))};
}
} // namespace quidra::ir
