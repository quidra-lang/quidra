#pragma once

// FunctionPrepass: the pass over one function before any of its text is
// written. It records the storage of the parameters, then visits every
// instruction once, in block order: it records the storage the instruction
// gives the function (SymbolTable::record_storage) and hands the
// instruction to a reservation callback, through which FunctionEmitter lets
// the emitter of the instruction's domain reserve what the instruction
// needs: entry-frame scratch slots (ScratchPlanner) and string pool names
// (StringPool).
//
// Owns nothing. The reservations number the scratch slots (%scratch.N) and
// the pool names (.str.N) in this visit order, which is part of the output.
// A function's pre-pass runs right before the function is emitted, never
// for the whole module first, so the pool names one function reserves
// follow the names the functions before it reserved and used. A reservation
// can throw (a backward pass without a gradient target), which ends the
// pass at that instruction.

#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"

#include <variant>

namespace quidra::llvm_backend {

// An emitter that reserves something for instructions of kind T before the
// function's text is written: it has reserve(const T&, const ir::Instruction&).
template <class Emitter,class T>
concept reserves_before_emission=requires(Emitter& emitter,const T& n,const ir::Instruction& ins){
    emitter.reserve(n,ins);
};

class FunctionPrepass {
public:
    FunctionPrepass(const ir::Function& fn,SymbolTable& symbols):fn_(fn),symbols_(symbols){}

    // Calls reserve(n, instruction) for every instruction, with n its
    // alternative, once the instruction's storage is recorded.
    template <class Reserve>
    void run(Reserve&& reserve){
        for(const auto& p:fn_.parameters) symbols_.declare_parameter(p);
        for(const auto& b:fn_.blocks){
            for(const auto& i:b.instructions){
                symbols_.record_storage(i);
                std::visit([&](const auto& n){reserve(n,i);},i);
            }
        }
    }

private:
    const ir::Function& fn_;
    SymbolTable& symbols_;
};

} // namespace quidra::llvm_backend
