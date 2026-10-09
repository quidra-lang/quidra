#pragma once

// EnclosingClass: the class whose method or constructor is being lowered
// (empty while a function or the entry is lowered), and whether the body is
// a constructor's; load_receiver loads that method's or constructor's
// receiver (enclosing_class.cpp).

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "quidra/standard_classes.hpp"
#include <string>

namespace quidra::lowering {

class EnclosingClass {
public:
    const std::string& name() const { return name_; }
    // `standard_library`: the class's ClassTypeInfo::standard_library.
    void enter(const std::string& class_name, bool standard_library) {
        name_ = class_name;
        standard_library_ = standard_library;
    }
    void leave() { name_.clear(); standard_library_ = false; }

    // Set while lowering a construct(...) body: `return` without a value and
    // falling off the end both return the receiver, and an error return
    // releases it first.
    bool in_constructor() const { return in_constructor_; }
    void set_in_constructor(bool value) { in_constructor_ = value; }

    // Whether the class is a standard Map or Set specialization, whose
    // methods the lowering treats as part of the collection.
    bool is_standard_collection() const {
        return standard_class::is_map_instance(name_, standard_library_) ||
               standard_class::is_set_instance(name_, standard_library_);
    }

private:
    std::string name_;
    bool standard_library_{};
    bool in_constructor_{};
};

// The receiver of the method or constructor being lowered: its $receiver
// local, loaded with the enclosing class's type.
ValueId load_receiver(ir::FunctionBuilder& builder, const EnclosingClass& enclosing_class);

} // namespace quidra::lowering
