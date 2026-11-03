#pragma once

// LocalScope: the source locals and references of the function being
// lowered, and the IR locals and references that hold them.
//
// A source name maps to the IR local (or IR reference) that stores it, and
// every IR local and reference has a type. Binding a source name again
// rebinds it: every binding is an assignment, and the last one wins.
//
// A branch, loop body or match case starts from the scope its construct
// started with: LocalNamesSnapshot saves the IR locals and the source local
// names and references, and restore() brings them back.
// The main guard saves and restores all four maps (FullScopeSnapshot).
// Restores are explicit calls, never destructors.
//
// local_type(ir_name) is the type slot of an IR local, reached through
// operator[]: assigning to it declares a hidden IR local (the last assignment
// wins), and reading it inserts a default Type for a name the scope does not
// know yet. The *_at() reads require the name to be known.

#include "ir/function_builder.hpp"
#include "quidra/types.hpp"
#include <string>
#include <functional>
#include <unordered_map>
#include <utility>

namespace quidra::lowering {

class LocalScope {
public:
    void binding_observer(std::function<void(const std::string&,const std::string&)> observer) {binding_observer_=std::move(observer);}
    struct ScaledReal {
        std::string numerator,denominator,general,start,step,counter;
        bool exclusive_start{};
        std::string machine_numerator,machine_flag;
    };
    struct BitPlace { ir::ValueId array; ir::ValueId index; };
    struct LocalNamesSnapshot {
        std::unordered_map<std::string, Type> locals;
        std::unordered_map<std::string, std::string> local_names;
        std::unordered_map<std::string, Type> reference_types;
        std::unordered_map<std::string, std::string> reference_bindings;
        std::unordered_map<std::string, BitPlace> bit_places;
        std::unordered_map<std::string, ir::ValueId> values;
        std::unordered_map<std::string, ScaledReal> scaled_reals;
    };
    struct FullScopeSnapshot {
        std::unordered_map<std::string, Type> locals;
        std::unordered_map<std::string, std::string> local_names;
        std::unordered_map<std::string, BitPlace> bit_places;
        std::unordered_map<std::string, ir::ValueId> values;
        std::unordered_map<std::string, ScaledReal> scaled_reals;
        std::unordered_map<std::string, Type> references;
        std::unordered_map<std::string, std::string> reference_names;
    };

    // Binding a source name takes the next hidden name of `builder` for its IR
    // local ($local.NAME.N) or reference ($ref.NAME.N), and returns it.
    std::string bind_source_local(ir::FunctionBuilder& builder, const std::string& source_name, const Type& type) {
        const auto ir_name = builder.hidden("local." + source_name);
        bind_local(source_name, ir_name, type);
        return ir_name;
    }
    // A shape variable (L17) is a read-only nat local, $shape.NAME.N.
    std::string bind_shape_variable(ir::FunctionBuilder& builder, const std::string& source_name, const Type& type) {
        const auto ir_name = builder.hidden("shape." + source_name);
        bind_local(source_name, ir_name, type);
        return ir_name;
    }
    std::string bind_source_reference(ir::FunctionBuilder& builder, const std::string& source_name, const Type& type) {
        const auto ir_name = builder.hidden("ref." + source_name);
        bind_reference(source_name, ir_name, type);
        return ir_name;
    }
    // A parameter, or a constructor's receiver: the IR local has the source
    // name.
    void bind_parameter(const std::string& name, const Type& type) {
        locals_[name] = type;
        local_names_[name] = name;
    }

    void bind_scaled_real(const std::string& name,ScaledReal value) { scaled_reals_[name]=std::move(value); }
    void erase_scaled_real(const std::string& name) { scaled_reals_.erase(name); }
    const ScaledReal* scaled_real(const std::string& name) const {
        const auto it=scaled_reals_.find(name);return it==scaled_reals_.end()?nullptr:&it->second;
    }
    void bind_source_value(const std::string& name,ir::ValueId value) { values_[name]=value; }
    const ir::ValueId* source_value(const std::string& name) const {
        const auto it=values_.find(name);
        return it==values_.end()?nullptr:&it->second;
    }
    void bind_bit_place(const std::string& name, ir::ValueId array, ir::ValueId index) {
        bit_places_[name] = BitPlace{array,index};
    }
    const BitPlace* bit_place(const std::string& name) const {
        const auto it=bit_places_.find(name);
        return it==bit_places_.end()?nullptr:&it->second;
    }
    void bind_capture(const std::string& source,const std::string& storage,const Type& type,bool reference) {
        if(reference) bind_reference(source,storage,type); else bind_local(source,storage,type);
    }
    bool has_source_local(const std::string& source_name) const { return local_names_.contains(source_name); }
    const std::string& source_local(const std::string& source_name) const { return local_names_.at(source_name); }
    const Type& source_local_type(const std::string& source_name) const { return locals_.at(source_local(source_name)); }
    bool is_source_reference(const std::string& source_name) const { return reference_names_.contains(source_name); }
    const std::string& source_reference(const std::string& source_name) const { return reference_names_.at(source_name); }

    Type& local_type(const std::string& ir_name) { return locals_[ir_name]; }
    const Type& local_type_at(const std::string& ir_name) const { return locals_.at(ir_name); }
    const Type& reference_type_at(const std::string& ir_name) const { return references_.at(ir_name); }

    const std::string& value_address(ir::FunctionBuilder& builder,ir::ValueId value,const Type& type) {
        auto [it,inserted]=value_addresses_.try_emplace(value);
        if(inserted) it->second=builder.hidden("for.index.address");
        locals_[it->second]=type;
        return it->second;
    }
    void reset() {
        locals_.clear();
        local_names_.clear();
        reference_names_.clear();
        references_.clear();
        bit_places_.clear();
        values_.clear();
        scaled_reals_.clear();
        value_addresses_.clear();
    }

    LocalNamesSnapshot snapshot_local_names() const { return LocalNamesSnapshot{locals_, local_names_, references_, reference_names_, bit_places_,values_,scaled_reals_}; }
    void restore(const LocalNamesSnapshot& snapshot) {
        locals_ = snapshot.locals;
        local_names_ = snapshot.local_names;
        references_ = snapshot.reference_types;
        reference_names_ = snapshot.reference_bindings;
        bit_places_ = snapshot.bit_places;
        values_ = snapshot.values;
        scaled_reals_ = snapshot.scaled_reals;
    }
    FullScopeSnapshot snapshot_full() const {
        return FullScopeSnapshot{locals_, local_names_, bit_places_,values_,scaled_reals_, references_, reference_names_};
    }
    void restore(FullScopeSnapshot&& snapshot) {
        locals_ = std::move(snapshot.locals);
        local_names_ = std::move(snapshot.local_names);
        references_ = std::move(snapshot.references);
        reference_names_ = std::move(snapshot.reference_names);
        bit_places_ = std::move(snapshot.bit_places);
        values_ = std::move(snapshot.values);
        scaled_reals_ = std::move(snapshot.scaled_reals);
    }

private:
    // Binds `source_name` to the IR local `ir_name` of `type`.
    void bind_local(const std::string& source_name, const std::string& ir_name, const Type& type) {
        if(binding_observer_)binding_observer_(ir_name,source_name);
        local_names_[source_name] = ir_name;
        locals_[ir_name] = type;
    }
    // Binds `source_name` to the IR reference `ir_name` of `type`.
    void bind_reference(const std::string& source_name, const std::string& ir_name, const Type& type) {
        if(binding_observer_)binding_observer_(ir_name,source_name);
        reference_names_[source_name] = ir_name;
        references_[ir_name] = type;
    }

    std::function<void(const std::string&,const std::string&)> binding_observer_;
    std::unordered_map<std::string, ScaledReal> scaled_reals_;
    std::unordered_map<ir::ValueId,std::string> value_addresses_;
    std::unordered_map<std::string, BitPlace> bit_places_;
    std::unordered_map<std::string, ir::ValueId> values_;
    std::unordered_map<std::string, Type> locals_;
    std::unordered_map<std::string, std::string> local_names_;
    std::unordered_map<std::string, std::string> reference_names_;
    std::unordered_map<std::string, Type> references_;
};

} // namespace quidra::lowering
