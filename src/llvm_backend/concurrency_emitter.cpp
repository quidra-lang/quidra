#include "llvm_backend/concurrency_emitter.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include <stdexcept>
#include <map>
#include "quidra/abi/symbols.hpp"
namespace quidra::llvm_backend {
using namespace llvm_text;
using namespace llvm_text::types;
void ConcurrencyEmitter::reserve(const ir::ParallelBlock& n,const ir::Instruction& ins) {
    scratch_.reserve(ins,LlvmType::array(n.nodes.size(),ptr),Align::none);
    for(const auto& node:n.nodes)scratch_.reserve(ins,LlvmType::array(node.captures.size()+1,ptr),Align::none);
    if(n.iterations && !n.iterations->arrays.empty())
        scratch_.reserve(ins,LlvmType::array(n.iterations->arrays.size(),ptr),Align::none);
}
void ConcurrencyEmitter::emit(const ir::ParallelBlock& n,const ir::Instruction& ins) {
    const auto& frames=scratch_.instruction_slot(ins);
    std::map<std::size_t,std::string> receipt_storage;
    std::string worker_safe=n.nodes.empty()||!n.nodes.front().worker_safe?"false":"true";
    if(n.iterations) {
        if(n.nodes.size()!=1 || n.nodes.front().fallible || !n.nodes.front().predecessors.empty())
            throw std::logic_error("parallel iterations require one nonfallible independent node");
        if(n.iterations->worker_safe)worker_safe=symbols_.value(*n.iterations->worker_safe);
        for(std::size_t i=0;i<n.iterations->arrays.size();++i) {
            const auto& array=n.iterations->arrays[i];
            const auto receipt=names_.value("parallel.array.receipt"),slot=names_.value("parallel.array.receipt.slot");
            builder_.call(receipt,runtime_abi::concurrency::array_prepare,
                {{ptr,symbols_.value(array.array)},{i1,array.read_only?"true":"false"},
                 {i1,array.bare_integer?"true":"false"},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            builder_.getelementptr(slot,Inbounds::no,ptr,scratch_.instruction_slot(ins,n.nodes.size()+1),{{i64,i}});
            builder_.store({ptr,receipt},slot,Align::none);receipt_storage.emplace(array.capture,slot);
            if(array.read_only && array.bare_integer) {
                const auto inline_values=names_.value("parallel.array.inline"),safe=names_.value("parallel.worker.safe");
                builder_.call(inline_values,runtime_abi::concurrency::array_inline,{{ptr,receipt}});
                builder_.binary(safe,BinaryOp::and_,i1,worker_safe,inline_values);worker_safe=safe;
            }
        }
    }
    for(std::size_t i=0;i<n.nodes.size();++i){
        const auto& frame=scratch_.instruction_slot(ins,i+1);
        builder_.store({ptr,"null"},frame,Align::none);
        for(std::size_t j=0;j<n.nodes[i].captures.size();++j){
            const auto slot=names_.value("parallel.capture");
            builder_.getelementptr(slot,Inbounds::no,ptr,frame,{{i64,j+1}});
            const auto& capture=n.nodes[i].captures[j];
            if(const auto receipt=receipt_storage.find(j);receipt!=receipt_storage.end())
                builder_.store({ptr,receipt->second},slot,Align::none);
            else {
                if(!capture.writable_address || !*capture.writable_address)throw std::logic_error("parallel capture lacks storage address");
                builder_.store({ptr,symbols_.value(*capture.writable_address)},slot,Align::none);
            }
        }
        const auto slot=names_.value("parallel.frame");
        builder_.getelementptr(slot,Inbounds::no,ptr,frames,{{i64,i}});
        builder_.store({ptr,frame},slot,Align::none);
    }
    const auto table=abi::user_symbol(n.table_name),raw=names_.value("parallel.result");
    if(n.iterations) {
        const auto captures=names_.value("parallel.iteration.captures");
        builder_.getelementptr(captures,Inbounds::no,ptr,scratch_.instruction_slot(ins,1),{{i64,1}});
        builder_.call(raw,runtime_abi::concurrency::parallel_iterations,
            {{ptr,LlvmOperand::global(table+".pointers")},{ptr,LlvmOperand::global(table+".numbers")},
             {ptr,captures},{i32,n.nodes.front().captures.size()},{i64,symbols_.value(n.iterations->count)},
             {i1,worker_safe},{ptr,LlvmOperand::global(".quidra.parallel.state")},
             {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    } else builder_.call(raw,runtime_abi::concurrency::parallel_block,
        {{ptr,LlvmOperand::global(table+".pointers")},{ptr,LlvmOperand::global(table+".numbers")},
         {ptr,frames},{i32,n.nodes.size()},{ptr,LlvmOperand::global(".quidra.parallel.state")},
         {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    const auto type=Type::union_of({Type::simple(TypeKind::Void),Type::simple(TypeKind::Error)});
    symbols_.set_type(n.out,type);
    const auto empty=names_.value("parallel.empty"),success=names_.label("parallel.success"),failure=names_.label("parallel.failure"),done=names_.label("parallel.joined");
    builder_.icmp(empty,IntPredicate::eq,ptr,raw,"null");builder_.br(empty,success,failure);
    builder_.block(success);
    const auto okay=names_.value("parallel.void");boxes_.allocate(okay,type);boxes_.store_tag(okay,case_index(type,Type::simple(TypeKind::Void)));
    builder_.br(done);builder_.block(failure);builder_.br(done);builder_.block(done);
    builder_.phi(symbols_.value(n.out),ptr,{{okay,success},{raw,failure}});
    symbols_.acquire_owned(n.out,type,symbols_.value(n.out));
}
}
