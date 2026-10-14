#pragma once

// Typed-IR instructions of the memory domain (locals, references, addresses, loads and stores).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
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
struct AddressReferenceSlot { ValueId out; std::string name; };
template <> struct InstructionTraits<AddressReferenceSlot> : WithOperands<Domain::memory, AddressReferenceSlot> {};
struct AddressLocal { ValueId out; std::string name; };
struct AddressField { ValueId out; ValueId object; std::size_t index; };
struct AddressElement { ValueId out; ValueId array; ValueId index; Type array_type; Type element_type; bool bin_element{}; SourcePosition position{}; bool bounds_proven{}; };
struct LoadAddress { ValueId out; ValueId address; Type type; };
struct StoreAddress { ValueId address; ValueId value; Type type; };
struct BindReference { std::string name; ValueId address; };
struct ReferenceAddress { ValueId out; std::string name; };
struct LoadReference { ValueId out; std::string name; Type type; };
struct StoreReference { std::string name; ValueId value; Type type; };
struct LoadLocal {
    ValueId out;
    std::string name;
    Type type;
    // Transfers an owned local to a call without an extra copy.
    bool move_out{};
};
struct StoreLocal {
    std::string name;
    ValueId value;
    Type type;
    bool borrowed{};
    bool replace_without_release{};
};

template <> struct InstructionTraits<DeclareLocal> : WithOperands<Domain::memory, DeclareLocal> {};
template <> struct InstructionTraits<DeclareReference> : WithOperands<Domain::memory, DeclareReference> {};
template <> struct InstructionTraits<AddressLocal> : WithOperands<Domain::memory, AddressLocal> {};
template <> struct InstructionTraits<AddressField> : WithOperands<Domain::memory, AddressField> {};
template <> struct InstructionTraits<AddressElement> : WithOperands<Domain::memory, AddressElement> {};
template <> struct InstructionTraits<LoadAddress> : WithOperands<Domain::memory, LoadAddress> {};
template <> struct InstructionTraits<StoreAddress> : WithOperands<Domain::memory, StoreAddress> {};
template <> struct InstructionTraits<BindReference> : WithOperands<Domain::memory, BindReference> {};
template <> struct InstructionTraits<ReferenceAddress> : WithOperands<Domain::memory, ReferenceAddress> {};
template <> struct InstructionTraits<LoadReference> : WithOperands<Domain::memory, LoadReference> {};
template <> struct InstructionTraits<StoreReference> : WithOperands<Domain::memory, StoreReference> {};
template <> struct InstructionTraits<LoadLocal> : WithOperands<Domain::memory, LoadLocal> {};
template <> struct InstructionTraits<StoreLocal> : WithOperands<Domain::memory, StoreLocal> {};

} // namespace quidra::ir
