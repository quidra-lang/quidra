#pragma once

// SymbolTable: the names and types of one function's storage.
//
// Owns: the type of every IR value (%vN), the alloca type of every local
// (%local.X) and reference, which parameters are passed by address (%arg.X),
// and which parameters and locals hold borrowed values that the function
// does not release.
//
// The verbs follow the container idioms the output depends on. set_type,
// declare_parameter and record_storage assign, so the last write wins: the
// last store to a local decides the type of its entry-frame alloca. type_at
// reads with .at and throws on an unknown value. Emitters keep the reference
// type_at returns while set_type inserts another value; the unordered_map is
// node-based, so a rehash leaves the reference valid, and it must never move
// to dense storage. locals and references are std::map: the entry frame and
// the cleanup at function exits walk them in name order.

#include "quidra/ir/module.hpp"
#include "llvm_text/escape.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace quidra::llvm_backend {

class SymbolTable {
public:
    std::string value(ir::ValueId id)const{return "%v"+std::to_string(id);}
    std::string local(const std::string&n)const{return "%local."+llvm_text::sanitize_identifier(n);}
    std::string arg(const std::string&n)const{return "%arg."+llvm_text::sanitize_identifier(n);}
    std::string storage(const std::string&n)const{return writable_params_.contains(n)?arg(n):local(n);}

    void set_type(ir::ValueId id,const Type& type){values_[id]=type;}
    void set_type(ir::ValueId id,Type&& type){values_[id]=std::move(type);}
    const Type& type_at(ir::ValueId id)const{return values_.at(id);}

    // The storage a parameter or an instruction (StoreLocal, DeclareLocal,
    // DeclareReference) gives the function; called in function order before
    // any instruction is emitted. Defined in the class, like the rest of the
    // table: the pre-pass calls record_storage for every instruction.
    void declare_parameter(const ir::Parameter& p){
        locals_[p.name]=p.type;
        if(p.writable) writable_params_.insert(p.name);
        if(p.borrowed) borrowed_params_.insert(p.name);
    }
    void record_storage(const ir::Instruction& i){
        if(const auto* s=std::get_if<ir::StoreLocal>(&i)){
            locals_[s->name]=s->type;
            if(s->borrowed) borrowed_locals_.insert(s->name);
        }
        if(const auto* d=std::get_if<ir::DeclareLocal>(&i)) locals_[d->name]=d->type;
        if(const auto* r=std::get_if<ir::DeclareReference>(&i)) references_[r->name]=r->type;
    }

    const std::map<std::string,Type>& locals()const{return locals_;}
    const std::map<std::string,Type>& references()const{return references_;}
    bool is_writable_parameter(const std::string& name)const{return writable_params_.contains(name);}
    bool is_borrowed_parameter(const std::string& name)const{return borrowed_params_.contains(name);}
    bool is_borrowed_local(const std::string& name)const{return borrowed_locals_.contains(name);}

private:
    std::unordered_map<ir::ValueId,Type> values_;
    std::map<std::string,Type> locals_;
    std::map<std::string,Type> references_;
    std::unordered_set<std::string> writable_params_;
    std::unordered_set<std::string> borrowed_params_;
    std::unordered_set<std::string> borrowed_locals_;
};

} // namespace quidra::llvm_backend
