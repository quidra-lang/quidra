#pragma once

// Typed-IR instructions of the memory domain (locals, references, addresses, loads and stores).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace quidra::ir {

struct DeclareLocal {
    std::string name;
    Type type;
    std::string source_name;
    std::uint32_t source_line{};
    std::uint32_t source_column{};
};
struct DeclareReference { std::string name; Type type; bool is_const{}; };
struct AddressLocal { ValueId out; std::string name; };
struct AddressField { ValueId out; ValueId object; std::size_t index; };
struct AddressElement { ValueId out; ValueId array; ValueId index; Type array_type; Type element_type; bool bin_element{}; std::uint32_t line{}; std::uint32_t column{}; };
struct LoadAddress { ValueId out; ValueId address; Type type; };
struct StoreAddress { ValueId address; ValueId value; Type type; };
struct BindReference { std::string name; ValueId address; };
struct ReferenceAddress { ValueId out; std::string name; };
struct LoadReference { ValueId out; std::string name; Type type; };
struct StoreReference { std::string name; ValueId value; Type type; };
struct LoadLocal { ValueId out; std::string name; Type type; };
struct StoreLocal {
    std::string name;
    ValueId value;
    Type type;
    bool borrowed{};
    bool replace_without_release{};
};

template <> struct InstructionTraits<DeclareLocal> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<DeclareReference> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<AddressLocal> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<AddressField> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<AddressElement> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<LoadAddress> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<StoreAddress> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<BindReference> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<ReferenceAddress> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<LoadReference> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<StoreReference> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<LoadLocal> : InDomain<Domain::memory> {};
template <> struct InstructionTraits<StoreLocal> : InDomain<Domain::memory> {};

} // namespace quidra::ir
