#include "ir/canonical_encoding.hpp"
#include "ir/content_hash.hpp"

#include <limits>
#include <utility>

namespace quidra::ir::canonical_detail {
std::int64_t ShapeSymbols::symbol(std::int64_t id) {
    if (const auto found = ids_.find(id); found != ids_.end()) return found->second;
    if (std::cmp_greater_equal(ids_.size(),std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("too many IR shape symbols");
    const auto normalized = static_cast<std::int64_t>(ids_.size()) + 1;
    ids_.emplace(id,normalized);
    return normalized;
}
ShapeExtent ShapeSymbols::normalize(const ShapeExtent& input) { return normalize(input,0); }
ShapeExtent ShapeSymbols::normalize(const ShapeExtent& input, std::size_t depth) {
    if (depth >= 128) throw std::invalid_argument("IR shape nesting exceeds 128");
    ShapeExtent result = input;
    // A binding pins the complete nonaggregate schema; no inactive field is
    // thrown away by rebuilding the expression through its semantic helpers.
    auto& [kind,value,symbols,terms,operands,atom_operands,atom_operations] = result;
    if (kind == ShapeExtent::Kind::Symbol) value = symbol(value);
    for (auto& id : symbols) id = symbol(id);
    if (std::is_sorted(input.symbols.begin(),input.symbols.end()))
        std::sort(symbols.begin(),symbols.end());
    for (std::size_t i = 0; i < operands.size(); ++i)
        operands[i] = normalize(input.operands[i],depth + 1);
    for (auto& [key,values] : atom_operands) {
        (void)key;
        for (auto& extent : values) extent = normalize(extent,depth + 1);
    }
    // Factors are string keys in the existing typed polynomial model. Match
    // only exact registered symbol/atom keys; arbitrary text is never parsed
    // or discarded. The atom operation and every operand remain encoded too.
    std::map<std::string,std::string> keys;
    for (const auto& [original,renamed] : ids_)
        keys.emplace(canonical_extent_key(ShapeExtent::symbol(original)),
                     canonical_extent_key(ShapeExtent::symbol(renamed)));
    const auto broadcast_key = [](const std::vector<ShapeExtent>& values, bool symbols_only) {
        ShapeExtent extent;
        extent.kind = ShapeExtent::Kind::Broadcast;
        if (symbols_only) {
            for (const auto& part : values) {
                if (part.kind != ShapeExtent::Kind::Symbol) return std::string{};
                extent.symbols.push_back(part.value);
            }
        } else extent.operands = values;
        return canonical_extent_key(extent);
    };
    for (const auto& [key,original_values] : input.atom_operands) {
        const auto operation = input.atom_operations.find(key);
        if (operation == input.atom_operations.end()) continue;
        auto& renamed_values = atom_operands.at(key);
        if ((operation->second == "/" || operation->second == "%") && original_values.size() == 2) {
            const auto original_key = operation->second + "(" + canonical_extent_key(original_values[0]) +
                                      "," + canonical_extent_key(original_values[1]) + ")";
            if (key == original_key)
                keys[key] = operation->second + "(" + canonical_extent_key(renamed_values[0]) +
                            "," + canonical_extent_key(renamed_values[1]) + ")";
        } else if (operation->second == "broadcast") {
            const bool symbols_only = key == broadcast_key(original_values,true);
            if (!symbols_only && key != broadcast_key(original_values,false)) continue;
            const auto less = [](const ShapeExtent& a, const ShapeExtent& b) {
                return canonical_extent_key(a) < canonical_extent_key(b);
            };
            if (std::is_sorted(original_values.begin(),original_values.end(),less))
                std::sort(renamed_values.begin(),renamed_values.end(),less);
            keys[key] = broadcast_key(renamed_values,symbols_only);
        }
    }
    const auto rename_key = [&](const std::string& key) {
        const auto found = keys.find(key);
        return found == keys.end() ? key : found->second;
    };
    for (std::size_t i = 0; i < terms.size(); ++i) {
        for (auto& factor : terms[i].factors) factor = rename_key(factor);
        if (std::is_sorted(input.terms[i].factors.begin(),input.terms[i].factors.end()))
            std::sort(terms[i].factors.begin(),terms[i].factors.end());
    }
    const auto term_less = [](const ShapeTerm& a, const ShapeTerm& b) { return a.factors < b.factors; };
    if (std::is_sorted(input.terms.begin(),input.terms.end(),term_less))
        std::stable_sort(terms.begin(),terms.end(),term_less);
    const auto extent_less = [](const ShapeExtent& a, const ShapeExtent& b) {
        return canonical_extent_key(a) < canonical_extent_key(b);
    };
    if (kind == ShapeExtent::Kind::Broadcast &&
        std::is_sorted(input.operands.begin(),input.operands.end(),extent_less))
        std::sort(operands.begin(),operands.end(),extent_less);
    decltype(atom_operands) renamed_operands;
    for (auto& [key,values] : atom_operands) {
        if (!renamed_operands.emplace(rename_key(key),std::move(values)).second)
            throw std::invalid_argument("shape atom normalization produced duplicate keys");
    }
    atom_operands = std::move(renamed_operands);
    decltype(atom_operations) renamed_operations;
    for (const auto& [key,operation] : atom_operations) {
        if (!renamed_operations.emplace(rename_key(key),operation).second)
            throw std::invalid_argument("shape atom normalization produced duplicate operations");
    }
    atom_operations = std::move(renamed_operations);
    return result;
}
} // namespace quidra::ir::canonical_detail

namespace quidra::ir {
namespace {
class LocationRewriter {
public:
    explicit LocationRewriter(const LocationTable* table) : table_(table) {}
    template <class T> void walk(T& value) {
        using V = std::remove_cvref_t<T>;
        Depth depth(depth_);
        if constexpr (std::is_same_v<V,SourcePosition>) {
            slot(LocationSlotKind::Position);
            value = table_ ? take(table_->positions,positions_) : SourcePosition{};
        } else if constexpr (std::is_same_v<V,SourceLocation>) {
            slot(LocationSlotKind::Location);
            value = table_ ? take(table_->locations,locations_) : SourceLocation{};
            fields::for_each_field(value,[&](auto, auto& field) {
                using F = std::remove_cvref_t<decltype(field)>;
                if constexpr (std::is_same_v<F,SourcePosition>) {
                    const auto original = field;
                    walk(field);
                    if (table_ && (field.line != original.line || field.column != original.column))
                        throw std::invalid_argument("inconsistent nested source position");
                } else static_assert(std::is_same_v<F,std::string>,
                                     "classify the new SourceLocation field explicitly");
            });
        } else if constexpr (std::is_same_v<V,SourceSpan>) {
            slot(LocationSlotKind::Span);
            value = table_ ? take(table_->spans,spans_) : SourceSpan{{0,0,0},{0,0,0}};
        } else if constexpr (std::is_same_v<V,SourcePos>) {
            slot(LocationSlotKind::SourcePos);
            value = table_ ? take(table_->source_positions,source_positions_) : SourcePos{0,0,0};
        } else if constexpr (canonical_detail::IsOptional<V>::value) {
            if (value) walk(*value);
        } else if constexpr (canonical_detail::IsVariant<V>::value) {
            if (value.valueless_by_exception()) throw std::invalid_argument("valueless IR variant");
            std::visit([&](auto& alternative) { walk(alternative); },value);
        } else if constexpr (canonical_detail::IsShared<V>::value) {
            if (value) {
                if (std::find(active_.begin(),active_.end(),value.get()) != active_.end())
                    throw std::invalid_argument("cyclic shared IR fields");
                active_.push_back(value.get());
                using E = std::remove_const_t<typename V::element_type>;
                auto copy = std::make_shared<E>(*value);
                walk(*copy);
                value = std::move(copy);
                active_.pop_back();
            }
        } else if constexpr (canonical_detail::IsVector<V>::value || canonical_detail::IsArray<V>::value) {
            if constexpr (!std::is_same_v<typename V::value_type,bool>)
                for (auto& item : value) walk(item);
        } else if constexpr (canonical_detail::IsMap<V>::value) {
            V rewritten(value.key_comp(),value.get_allocator());
            for (const auto& entry : value) {
                typename V::key_type key = entry.first;
                typename V::mapped_type item = entry.second;
                walk(key); walk(item);
                if (!rewritten.emplace(std::move(key),std::move(item)).second)
                    throw std::invalid_argument("source-neutral map keys collide");
            }
            value = std::move(rewritten);
        } else if constexpr (canonical_detail::IsSet<V>::value) {
            V rewritten(value.key_comp(),value.get_allocator());
            for (auto item : value) {
                walk(item);
                if (!rewritten.insert(std::move(item)).second)
                    throw std::invalid_argument("source-neutral set elements collide");
            }
            value = std::move(rewritten);
        } else if constexpr (canonical_detail::IsPair<V>::value) {
            walk(value.first); walk(value.second);
        } else if constexpr (std::is_aggregate_v<V> || std::is_same_v<V,Binary> ||
                             std::is_same_v<V,ShapeExtent>) {
            fields::for_each_field(value,[&](auto index, auto& field) {
                if constexpr (std::is_same_v<V,Function> && index == 1) {
                    slot(LocationSlotKind::FunctionFile);
                    field = table_ ? table_->source_file : std::string{};
                } else if constexpr (std::is_same_v<V,Function> && index == 2) {
                    slot(LocationSlotKind::FunctionLine);
                    field = table_ ? table_->function_position.line : 0;
                } else if constexpr (std::is_same_v<V,Function> && index == 3) {
                    slot(LocationSlotKind::FunctionColumn);
                    field = table_ ? table_->function_position.column : 0;
                } else if constexpr (std::is_same_v<V,InlineScope> && index == 4) {
                    slot(LocationSlotKind::SourceFile);
                    field = table_ ? take(table_->source_files,source_files_) : std::string{};
                } else if constexpr (std::is_same_v<V,InlineSlot> && index == 3) {
                    slot(LocationSlotKind::InlineSourceName);
                    field = table_ ? take(table_->source_names,source_names_) : std::string{};
                } else if constexpr (std::is_same_v<V,DeclarationIdentity> && index == 6) {
                    slot(LocationSlotKind::SourceFile);
                    field = table_ ? take(table_->source_files,source_files_) : std::string{};
                } else walk(field);
            });
        } else {
            static_assert(std::is_arithmetic_v<V> || std::is_enum_v<V> ||
                          std::is_same_v<V,std::string> || std::is_same_v<V,ShapeCoefficient>,
                          "unsupported IR field in source stitcher");
        }
    }
    void finish() const {
        if (table_ && (slots_ != table_->slots.size() || positions_ != table_->positions.size() ||
            locations_ != table_->locations.size() || spans_ != table_->spans.size() ||
            source_positions_ != table_->source_positions.size() || source_files_ != table_->source_files.size() ||
            source_names_ != table_->source_names.size()))
            throw std::invalid_argument("surplus IR location slots");
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
    const LocationTable* table_;
    std::size_t slots_{}, positions_{}, locations_{}, spans_{}, source_positions_{}, source_files_{}, source_names_{};
    std::size_t depth_{};
    std::vector<const void*> active_;
    void slot(LocationSlotKind kind) {
        if (!table_) return;
        if (slots_ >= table_->slots.size() || table_->slots[slots_] != kind)
            throw std::invalid_argument("IR location slot schema mismatch");
        ++slots_;
    }
    template <class T> static T take(const std::vector<T>& values, std::size_t& index) {
        if (index >= values.size()) throw std::invalid_argument("missing IR location slot");
        return values[index++];
    }
};
} // namespace

CanonicalEncoding canonical_encode(const Function& function) { return canonical_encode_value(function); }
std::string canonical_location_bytes(const LocationTable& table) {
    canonical_detail::Encoder encoder(true);
    encoder.put(table);
    return encoder.finish().bytes;
}
Function location_neutral(const Function& function) {
    Function result = function;
    LocationRewriter rewriter(nullptr);
    rewriter.walk(result);
    return result;
}
Function stitch(Function structural, const LocationTable& table) {
    LocationRewriter rewriter(&table);
    rewriter.walk(structural);
    rewriter.finish();
    return structural;
}
} // namespace quidra::ir
