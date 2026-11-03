#pragma once

#include "ir/field_walk.hpp"
#include "ir/facts/entry.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/ir/function.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::ir {

enum class LocationSlotKind : std::uint8_t {
    Position, Location, Span, SourcePos, SourceFile,
    FunctionFile, FunctionLine, FunctionColumn, InlineSourceName
};
// Slots follow the complete field traversal, including a SourceLocation's
// nested position. Its duplicated position must agree when stitching.
struct LocationTable {
    std::vector<SourcePosition> positions;
    std::vector<SourceLocation> locations;
    std::string source_file;
    SourcePosition function_position;
    std::vector<SourceSpan> spans;
    std::vector<SourcePos> source_positions;
    std::vector<std::string> source_files;
    std::vector<LocationSlotKind> slots;
    std::vector<std::string> source_names;
};
struct CanonicalEncoding {
    std::string bytes;
    LocationTable locations;
};

namespace canonical_detail {
template <class> inline constexpr bool unsupported = false;
template <class> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type {};
template <class> struct IsVariant : std::false_type {};
template <class... T> struct IsVariant<std::variant<T...>> : std::true_type {};
template <class> struct IsShared : std::false_type {};
template <class T> struct IsShared<std::shared_ptr<T>> : std::true_type {};
template <class> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T,A>> : std::true_type {};
template <class> struct IsArray : std::false_type {};
template <class T, std::size_t N> struct IsArray<std::array<T,N>> : std::true_type {};
template <class> struct IsMap : std::false_type {};
template <class K, class V, class C, class A>
struct IsMap<std::map<K,V,C,A>> : std::true_type {};
template <class> struct IsSet : std::false_type {};
template <class T, class C, class A> struct IsSet<std::set<T,C,A>> : std::true_type {};
template <class> struct IsPair : std::false_type {};
template <class A, class B> struct IsPair<std::pair<A,B>> : std::true_type {};

// ShapeExtent's seven actual fields are preserved. Only explicitly typed
// symbol IDs and exact keys linked to typed atoms are alpha-normalized.
class ShapeSymbols {
public:
    ShapeExtent normalize(const ShapeExtent&);
private:
    std::map<std::int64_t,std::int64_t> ids_;
    std::int64_t symbol(std::int64_t);
    ShapeExtent normalize(const ShapeExtent&, std::size_t depth);
};

enum class Tag : std::uint8_t {
    Boolean, Signed, Unsigned, Float32, Float64, String, Optional, Variant,
    Shared, Vector, Array, Set, Map, Pair, Aggregate, Coefficient,
    Position, Location, Span, SourcePos, SourceFile, FunctionFile,
    FunctionLine, FunctionColumn, InlineSourceName
};
class Encoder {
public:
    explicit Encoder(bool raw_locations = false) : raw_locations_(raw_locations) {
        result_.bytes = "quidra-ir/1";
    }
    CanonicalEncoding finish() { return std::move(result_); }
    template <class T> void put(const T& value) {
        using V = std::remove_cvref_t<T>;
        Depth depth(depth_);
        if constexpr (std::is_same_v<V,bool>) {
            tag(Tag::Boolean); byte(value ? 1 : 0);
        } else if constexpr (std::is_enum_v<V>) {
            put(static_cast<std::underlying_type_t<V>>(value));
        } else if constexpr (std::is_integral_v<V>) {
            static_assert(sizeof(V) <= sizeof(std::uint64_t), "integer encoding needs a wider schema");
            tag(std::is_signed_v<V> ? Tag::Signed : Tag::Unsigned);
            word(static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<V,float>) {
            tag(Tag::Float32); word(std::bit_cast<std::uint32_t>(value));
        } else if constexpr (std::is_same_v<V,double>) {
            tag(Tag::Float64); word(std::bit_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<V,std::string> || std::is_same_v<V,std::string_view>) {
            tag(Tag::String); word(value.size()); result_.bytes.append(value.data(),value.size());
        } else if constexpr (std::is_same_v<V,ShapeCoefficient>) {
            tag(Tag::Coefficient); put(value.text());
        } else if constexpr (std::is_same_v<V,SourcePosition>) {
            if (raw_locations_) aggregate(value);
            else {
                slot(Tag::Position,LocationSlotKind::Position,result_.locations.positions.size());
                result_.locations.positions.push_back(value);
            }
        } else if constexpr (std::is_same_v<V,SourceLocation>) {
            if (raw_locations_) aggregate(value);
            else {
                slot(Tag::Location,LocationSlotKind::Location,result_.locations.locations.size());
                result_.locations.locations.push_back(value);
                word(fields::aggregate_arity<V>());
                fields::for_each_field(value,[&](auto, const auto& field) {
                    using F = std::remove_cvref_t<decltype(field)>;
                    if constexpr (std::is_same_v<F,SourcePosition>) put(field);
                    else static_assert(std::is_same_v<F,std::string>,
                                       "classify the new SourceLocation field explicitly");
                });
            }
        } else if constexpr (std::is_same_v<V,SourceSpan>) {
            if (raw_locations_) aggregate(value);
            else {
                slot(Tag::Span,LocationSlotKind::Span,result_.locations.spans.size());
                result_.locations.spans.push_back(value);
            }
        } else if constexpr (std::is_same_v<V,SourcePos>) {
            if (raw_locations_) aggregate(value);
            else {
                slot(Tag::SourcePos,LocationSlotKind::SourcePos,result_.locations.source_positions.size());
                result_.locations.source_positions.push_back(value);
            }
        } else if constexpr (std::is_same_v<V,ShapeExtent>) {
            if (normalized_shapes_) aggregate(value);
            else {
                const auto normalized = shapes_.normalize(value);
                normalized_shapes_ = true;
                aggregate(normalized);
                normalized_shapes_ = false;
            }
        } else if constexpr (IsOptional<V>::value) {
            tag(Tag::Optional); byte(value ? 1 : 0);
            if (value) put(*value);
        } else if constexpr (IsVariant<V>::value) {
            if (value.valueless_by_exception()) throw std::invalid_argument("valueless IR variant");
            tag(Tag::Variant); word(std::variant_size_v<V>); word(value.index());
            std::visit([&](const auto& alternative) { put(alternative); },value);
        } else if constexpr (IsShared<V>::value) {
            tag(Tag::Shared); byte(value ? 1 : 0);
            if (value) {
                if (std::find(active_.begin(),active_.end(),value.get()) != active_.end())
                    throw std::invalid_argument("cyclic shared IR fields");
                active_.push_back(value.get());
                put(*value);
                active_.pop_back();
            }
        } else if constexpr (IsVector<V>::value || IsArray<V>::value || IsSet<V>::value) {
            if constexpr (IsVector<V>::value) tag(Tag::Vector);
            else if constexpr (IsArray<V>::value) tag(Tag::Array);
            else tag(Tag::Set);
            word(value.size());
            for (const auto& item : value) {
                if constexpr (std::is_same_v<typename V::value_type,bool>) put(static_cast<bool>(item));
                else put(item);
            }
        } else if constexpr (IsMap<V>::value) {
            tag(Tag::Map); word(value.size());
            for (const auto& [key,item] : value) { put(key); put(item); }
        } else if constexpr (IsPair<V>::value) {
            tag(Tag::Pair); put(value.first); put(value.second);
        } else if constexpr (std::is_aggregate_v<V> || std::is_same_v<V,Binary>) {
            aggregate(value);
        } else {
            static_assert(unsupported<V>, "unsupported IR field: add a complete typed adapter");
        }
    }
private:
    struct Depth {
        std::size_t& count;
        explicit Depth(std::size_t& value) : count(value) {
            if (count >= 256) throw std::invalid_argument("IR field nesting exceeds 256");
            ++count;
        }
        ~Depth() { --count; }
    };
    CanonicalEncoding result_;
    ShapeSymbols shapes_;
    std::vector<const void*> active_; // Cycle detection only; never encoded.
    std::size_t depth_{};
    bool raw_locations_{};
    bool normalized_shapes_{};
    void byte(std::uint8_t value) { result_.bytes.push_back(static_cast<char>(value)); }
    void tag(Tag value) { byte(static_cast<std::uint8_t>(value)); }
    void word(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) byte(static_cast<std::uint8_t>(value >> shift));
    }
    void slot(Tag type, LocationSlotKind kind, std::size_t index) {
        tag(type); word(index); result_.locations.slots.push_back(kind);
    }
    template <class T> void aggregate(const T& value) {
        using V = std::remove_cvref_t<T>;
        tag(Tag::Aggregate); word(fields::aggregate_arity<V>());
        fields::for_each_field(value,[&](auto index, const auto& field) {
            word(index);
            if constexpr (std::is_same_v<V,Function> && index == 1) {
                if (raw_locations_) put(field);
                else {
                    result_.locations.source_file = field;
                    slot(Tag::FunctionFile,LocationSlotKind::FunctionFile,0);
                }
            } else if constexpr (std::is_same_v<V,Function> && index == 2) {
                if (raw_locations_) put(field);
                else {
                    result_.locations.function_position.line = field;
                    slot(Tag::FunctionLine,LocationSlotKind::FunctionLine,0);
                }
            } else if constexpr (std::is_same_v<V,Function> && index == 3) {
                if (raw_locations_) put(field);
                else {
                    result_.locations.function_position.column = field;
                    slot(Tag::FunctionColumn,LocationSlotKind::FunctionColumn,0);
                }
            } else if constexpr (std::is_same_v<V,InlineScope> && index == 4) {
                if (raw_locations_) put(field);
                else {
                    slot(Tag::SourceFile,LocationSlotKind::SourceFile,result_.locations.source_files.size());
                    result_.locations.source_files.push_back(field);
                }
            } else if constexpr (std::is_same_v<V,InlineSlot> && index == 3) {
                if (raw_locations_) put(field);
                else {
                    slot(Tag::InlineSourceName,LocationSlotKind::InlineSourceName,result_.locations.source_names.size());
                    result_.locations.source_names.push_back(field);
                }
            } else if constexpr (std::is_same_v<V,DeclarationIdentity> && index == 6) {
                if (raw_locations_) put(field);
                else {
                    slot(Tag::SourceFile,LocationSlotKind::SourceFile,result_.locations.source_files.size());
                    result_.locations.source_files.push_back(field);
                }
            } else put(field);
        });
    }
};
} // namespace canonical_detail

// General typed values (including semantic helpers with SourceSpan) use the
// same field-complete schema as Function. Unknown types are compile errors.
template <class T> CanonicalEncoding canonical_encode_value(const T& value) {
    canonical_detail::Encoder encoder;
    encoder.put(value);
    return encoder.finish();
}
CanonicalEncoding canonical_encode(const Function&);
// Keeps every SourceLocation node and source-slot presence/order; only the
// attribution payload is cleared. Shared fields are copied before rewriting.
Function location_neutral(const Function&);
std::string canonical_location_bytes(const LocationTable&);
} // namespace quidra::ir
