#pragma once

// Array cast helpers: the LLVM functions a module defines for the numeric
// casts of arrays (ArrayNumericCast), one validator and one cast helper per
// source/target pair, nested element pairs included. The validator
// (@quidra_array_cast_validate_<from>_to_<to>) returns whether every
// element converts, which a fallible cast branches on; the cast helper
// (@quidra_array_cast_<from>_to_<to>) allocates the target array and
// converts each element.
//
// Owns: the helper symbols, the collection of the pairs a module needs and
// the text of both helpers. The collector keys its std::map by the pair's
// type identifiers, so the helpers are emitted in identifier order, once
// per pair; collecting throws for a cast that changes the structure of the
// array. TypeHelperSet runs them with the other type helpers.

#include "quidra/ir/module.hpp"
#include "llvm_backend/storage_layout.hpp"

#include <map>
#include <string>

namespace quidra::llvm_backend {

std::string array_cast_name(const Type&from,const Type&to);
std::string array_cast_validate_name(const Type&from,const Type&to);

struct ArrayCastPair {
    Type source;
    Type target;
};

std::map<std::string,ArrayCastPair> collect_array_cast_pairs(const ir::Module& module);
std::string emit_array_cast_validator(const Type& source, const Type& target,
                                      const ArrayLayoutPolicy& array_layout);
std::string emit_array_cast_helper(const Type& source, const Type& target,
                                   const ArrayLayoutPolicy& array_layout);

} // namespace quidra::llvm_backend
