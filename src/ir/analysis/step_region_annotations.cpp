#include "ir/analysis/step_region_annotations.hpp"
#include "ir/analysis/step_regions.hpp"
#include "ir/canonical_encoding.hpp"
#include "ir/summaries.hpp"
#include "ir/fact_sources.hpp"
#include "ir/binding_origins.hpp"
#include "semantics/interface_contracts.hpp"
#include "quidra/compiler_extension_model.hpp"

#include <algorithm>
#include <map>
#include <new>
#include <stdexcept>

namespace quidra::ir::analysis {
struct StepRegionAnnotationAccess {
    static std::shared_ptr<const StepRegionAnnotations> make(StepRegionAnnotationData data) {
        return std::shared_ptr<const StepRegionAnnotations>(new StepRegionAnnotations(std::move(data)));
    }
};
namespace {
struct Failure { StepRegionAnnotationGap gap; };
struct Work {
    std::uint64_t left, used{};
    std::size_t byte_limit, byte_used{};
    void take(std::uint64_t count = 1) {
        if (count > left) throw Failure{StepRegionAnnotationGap::WorkBudget};
        left -= count; used += count;
    }
    void bytes(std::size_t count) {
        if (count > byte_limit - byte_used) throw Failure{StepRegionAnnotationGap::ByteBudget};
        byte_used += count;
    }
};
// Complete raw authored fields. No symbol renaming, location normalization,
// digest, pointer address or existing annotation participates as a proof.
class ExactFields {
public:
  ExactFields(Work &work) : work_(work) {}
  std::string finish() { return std::move(bytes_); }
  template <class T> void put(const T &value) {
    using V = std::remove_cvref_t<T>;
    work_.take();
    if (depth_ >= 256)
      throw Failure{StepRegionAnnotationGap::DepthBudget};
    struct Depth {
      std::size_t &n;
      Depth(std::size_t &x) : n(x) { ++n; }
      ~Depth() { --n; }
    } depth(depth_);
    using namespace ir::canonical_detail;
    if constexpr (std::is_same_v<V, bool>)
      word(value ? 1 : 0);
    else if constexpr (std::is_enum_v<V>)
      put(static_cast<std::underlying_type_t<V>>(value));
    else if constexpr (std::is_integral_v<V>) {
      static_assert(sizeof(V) <= 8);
      word(static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_same_v<V, float>)
      word(std::bit_cast<std::uint32_t>(value));
    else if constexpr (std::is_same_v<V, double>)
      word(std::bit_cast<std::uint64_t>(value));
    else if constexpr (std::is_same_v<V, std::string> ||
                       std::is_same_v<V, std::string_view>) {
      word(value.size());
      append(value);
    } else if constexpr (std::is_same_v<V, ShapeCoefficient>)
      put(value.text());
    else if constexpr (IsOptional<V>::value) {
      word(value ? 1 : 0);
      if (value)
        put(*value);
    } else if constexpr (IsVariant<V>::value) {
      if (value.valueless_by_exception())
        throw std::invalid_argument("valueless annotation preimage");
      word(value.index());
      std::visit([&](const auto &node) { put(node); }, value);
    } else if constexpr (IsShared<V>::value) {
      word(value ? 1 : 0);
      if (value) {
        if (std::find(active_.begin(), active_.end(), value.get()) !=
            active_.end())
          throw std::invalid_argument("cyclic annotation preimage");
        active_.push_back(value.get());
        put(*value);
        active_.pop_back();
      }
    } else if constexpr (IsVector<V>::value || IsArray<V>::value ||
                         IsSet<V>::value) {
      word(value.size());
      for (const auto &item : value) {
        if constexpr (std::is_same_v<typename V::value_type, bool>)
          put(static_cast<bool>(item));
        else
          put(item);
      }
    } else if constexpr (IsMap<V>::value) {
      word(value.size());
      for (const auto &[key, item] : value) {
        put(key);
        put(item);
      }
    } else if constexpr (IsPair<V>::value) {
      put(value.first);
      put(value.second);
    } else if constexpr (std::is_aggregate_v<V> || std::is_same_v<V, Binary> ||
                         std::is_same_v<V, ShapeExtent>) {
      word(ir::fields::aggregate_arity<V>());
      ir::fields::for_each_field(value,
                                 [&](auto, const auto &field) { put(field); });
    } else
      static_assert(ir::canonical_detail::unsupported<V>,
                    "complete raw annotation preimage needs an authored adapter");
  }

private:
  Work &work_;
  std::size_t depth_{};
  std::string bytes_;
  std::vector<const void *> active_;
  void append(std::string_view text) {
    work_.bytes(text.size()); bytes_.append(text);
  }
  void word(std::uint64_t value) {
    work_.bytes(8);
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes_.push_back(static_cast<char>(value >> shift));
  }
};

template<class T> std::string raw(const T& value, Work& work) {
    ExactFields fields(work); fields.put(value); return fields.finish();
}
std::string environment(const Module& module, Work& work) {
    ExactFields fields(work);
    fields.put(module.functions); fields.put(module.classes);
    fields.put(module.compiler_extensions); fields.put(module.user_sources);
    fields.put(module.declaration_catalog); fields.put(module.interface_sources);
    fields.put(module.fact_sources); fields.put(module.binding_origins);
    fields.put(static_cast<bool>(module.summaries));
    if (module.summaries) {
        const auto& snapshots = module.summaries->semantic_snapshots();
        fields.put(snapshots);
        fields.put(module.summaries->by_function.size());
        for (const auto& [name, row] : module.summaries->by_function) {
            work.take(); fields.put(name);
            fields.put(static_cast<const IrSummaryFacts&>(row));
            fields.put(row.semantic_key); fields.put(row.semantic_parameter_for_ir_parameter);
            fields.put(row.pure_tensor); fields.put(row.semantic != nullptr);
            if (row.semantic) {
                const auto owned = snapshots.find(row.semantic_key);
                if (owned == snapshots.end() || &owned->second != row.semantic)
                    throw Failure{StepRegionAnnotationGap::InvalidInput};
                // The verified semantic key represents the owned link.
                fields.put(owned->first);
            }
        }
    }
    return fields.finish();
}
// Detach mutable nested Type pointees before the const report owner is made.
// Field-complete traversal keeps future pointer-bearing report fields honest.
template<class T> void detach(T& value, Work& work, std::size_t depth = 0) {
    using V = std::remove_cvref_t<T>;
    using namespace ir::canonical_detail;
    work.take();
    if (depth >= 256) throw Failure{StepRegionAnnotationGap::DepthBudget};
    if constexpr (IsShared<V>::value) {
        if (value) {
            using Item = std::remove_const_t<typename V::element_type>;
            auto copy = std::make_shared<Item>(*value);
            detach(*copy, work, depth + 1); value = std::move(copy);
        }
    } else if constexpr (IsOptional<V>::value) {
        if (value) detach(*value, work, depth + 1);
    } else if constexpr (IsVariant<V>::value) {
        std::visit([&](auto& node) { detach(node, work, depth + 1); }, value);
    } else if constexpr (IsVector<V>::value || IsArray<V>::value) {
        for (auto& item : value) detach(item, work, depth + 1);
    } else if constexpr (IsMap<V>::value) {
        for (auto& [key, item] : value) { (void)key; detach(item, work, depth + 1); }
    } else if constexpr (IsSet<V>::value) {
        V copy; for (const auto& item : value) { auto next = item; detach(next, work, depth + 1); copy.insert(std::move(next)); }
        value = std::move(copy);
    } else if constexpr (IsPair<V>::value) {
        detach(value.first, work, depth + 1); detach(value.second, work, depth + 1);
    } else if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view> ||
                         std::is_same_v<V, ShapeCoefficient> || std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        // Owned terminal value; no shared mutable state.
    } else if constexpr (std::is_aggregate_v<V> || std::is_same_v<V, ShapeExtent> || std::is_same_v<V, Binary>) {
        ir::fields::for_each_field(value, [&](auto, auto& field) { detach(field, work, depth + 1); });
    } else static_assert(unsupported<V>, "detach every owned annotation field explicitly");
}
void unique_names(const Module& module, Work& work) {
    std::map<std::string, std::size_t, std::less<>> counts;
    for (const auto& function : module.functions) { work.take(); if (++counts[function.name] != 1) throw Failure{StepRegionAnnotationGap::AmbiguousLookup}; }
}
} // namespace
StepRegionAnnotationBuild build_step_region_annotations(const Module& module,
    StepRegionAnnotationOptions options) {
    Work work{options.maximum_operations, 0, options.maximum_encoded_bytes, 0};
    StepRegionAnnotationBuild result;
    try {
        work.take(); unique_names(module, work);
        StepRegionAnnotationData data; data.environment_fields = environment(module, work);
        for (std::size_t index = 0; index < module.functions.size(); ++index) {
            work.take(); const auto& function = module.functions[index];
            StepRegionAnnotationRow row; row.function = index; row.lookup = function.name;
            work.bytes(row.lookup.size()); row.function_fields = raw(function, work);
            auto report = StepRegionFormation::analyze(module, index, {work.left});
            work.take(report.operations);
            if (report.budget_exhausted) throw Failure{StepRegionAnnotationGap::FormationBudget};
            if (!report.ssa_valid) throw Failure{StepRegionAnnotationGap::InvalidSsa};
            row.report_fields = raw(report, work);
            // Count the typed report's encoded payload in addition to its
            // stored raw guard. This is not an allocator/RSS bound.
            work.bytes(row.report_fields.size()); detach(report, work);
            row.report = std::make_shared<const StepRegionReport>(std::move(report));
            data.rows.push_back(std::move(row));
        }
        data.operations = work.used; data.encoded_bytes = work.byte_used;
        result.owner = StepRegionAnnotationAccess::make(std::move(data));
    } catch (const Failure& failure) { result.gap = failure.gap; }
      catch (const std::bad_alloc&) { result.gap = StepRegionAnnotationGap::Allocation; }
      catch (const std::length_error&) { result.gap = StepRegionAnnotationGap::ByteBudget; }
      catch (const std::logic_error&) { result.gap = StepRegionAnnotationGap::InvalidInput; }
    result.operations = work.used; result.encoded_bytes = work.byte_used; return result;
}
StepRegionAnnotationRead read_step_region_annotation(const Module& module,
    std::size_t function, StepRegionAnnotationOptions options) {
    Work work{options.maximum_operations, 0, options.maximum_encoded_bytes, 0};
    StepRegionAnnotationRead result;
    try {
        work.take();
        if (!module.step_regions) throw Failure{StepRegionAnnotationGap::Missing};
        if (function >= module.functions.size()) throw Failure{StepRegionAnnotationGap::InvalidFunction};
        const auto& data = module.step_regions->data();
        if (data.version != 1 || data.rows.size() != module.functions.size()) throw Failure{StepRegionAnnotationGap::InvalidInput};
        unique_names(module, work);
        const auto& row = data.rows[function];
        if (row.function != function || row.lookup != module.functions[function].name ||
            raw(module.functions[function], work) != row.function_fields)
            throw Failure{StepRegionAnnotationGap::StaleFunction};
        if (environment(module, work) != data.environment_fields) throw Failure{StepRegionAnnotationGap::StaleEnvironment};
        if (!row.report || raw(*row.report, work) != row.report_fields) throw Failure{StepRegionAnnotationGap::StaleReport};
        result.owner = module.step_regions; result.row = &row;
    } catch (const Failure& failure) { result.gap = failure.gap; }
      catch (const std::bad_alloc&) { result.gap = StepRegionAnnotationGap::Allocation; }
      catch (const std::length_error&) { result.gap = StepRegionAnnotationGap::ByteBudget; }
      catch (const std::logic_error&) { result.gap = StepRegionAnnotationGap::InvalidInput; }
    result.operations = work.used; result.encoded_bytes = work.byte_used; return result;
}
} // namespace quidra::ir::analysis
