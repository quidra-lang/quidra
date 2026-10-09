// FunctionEmitter: the LLVM text of one typed-IR function.
//
// emit() writes the function into one text buffer (a FunctionText, through
// its LlvmBuilder) in a single pass: the pre-pass (FunctionPrepass) first
// records the types of locals and references and lets each instruction's
// domain emitter reserve its string constants and entry-frame scratch slots
// (reserve); then come the definition header (an LlvmFunction), the entry
// frame, every block (emit_instruction per instruction) and, for self-depth
// recursion, the wrapper. emit_instruction
// hands each instruction to the emitter of its domain (emitter_for, from the
// domain table EmitterOf): a source location to DebugEmitter, every other
// instruction, within its debug segment, to its domain emitter
// (ConstantEmitter, ...). The debug locations recorded per statement are
// attached to the finished text by byte offset. The order of name allocation
// (the temporaries and labels of TemporaryNames, pool interning, debug ids)
// is part of the output.
//
// Owns the per-function state: the names and types of values and storage
// (SymbolTable), the entry-frame scratch slots (ScratchPlanner), the
// temporary names (TemporaryNames), where failures are reported and the
// calls that keep the user's statement current (StatementAttribution, the
// observer of every call written into the function's blocks) and the debug
// information (DebugEmitter); the services the domain emitters emit through: union
// boxes (UnionBox), releases (LifetimeEmitter), fail-fast guards
// (FailFastEmitter), recoverable error results (ErrorResultEmitter), REPL
// values (ReplValuePrinter) and call symbols (SymbolResolver); and the
// domain emitters, each built with references to the objects its handlers
// use. It writes the definition header under SymbolResolver's name for the
// function and wraps self-depth recursion with SelfDepthGuard. Shares the
// module's string pool, context (callable functions, class layouts, array layout
// policy, recursive functions: ModuleContext) and debug metadata
// (DebugModule).

#include "llvm_backend/function_emitter.hpp"

#include "llvm_backend/aggregate_emitter.hpp"
#include "llvm_backend/autograd_emitter.hpp"
#include "llvm_backend/bin_emitter.hpp"
#include "llvm_backend/call_emitter.hpp"
#include "llvm_backend/check_emitter.hpp"
#include "llvm_backend/concurrency_emitter.hpp"
#include "llvm_backend/console_emitter.hpp"
#include "llvm_backend/constant_emitter.hpp"
#include "llvm_backend/control_flow_emitter.hpp"
#include "llvm_backend/conversion_emitter.hpp"
#include "llvm_backend/debug_emitter.hpp"
#include "llvm_backend/error_result_emitter.hpp"
#include "llvm_backend/fail_fast_emitter.hpp"
#include "llvm_backend/file_emitter.hpp"
#include "llvm_backend/foreign_abi.hpp"
#include "llvm_backend/function_prepass.hpp"
#include "llvm_backend/http_emitter.hpp"
#include "llvm_backend/json_emitter.hpp"
#include "llvm_backend/lifetime_emitter.hpp"
#include "llvm_backend/memory_emitter.hpp"
#include "llvm_backend/numeric_emitter.hpp"
#include "llvm_backend/random_emitter.hpp"
#include "llvm_backend/repl_emitter.hpp"
#include "llvm_backend/repl_value_printer.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/self_depth_guard.hpp"
#include "llvm_backend/statement_attribution.hpp"
#include "llvm_backend/symbol_resolver.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/system_emitter.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/tensor_emitter.hpp"
#include "llvm_backend/text_emitter.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"

#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace quidra::llvm_backend {
namespace {

using namespace llvm_text;
using namespace llvm_text::types;

// False for every domain: the static_assert of a domain without an emitter.
template <ir::Domain> inline constexpr bool domain_without_emitter = false;

// The domain table: EmitterOf<D>::member is the member of FunctionEmitter
// that emits the instructions of domain D. Its entries, one per ir::Domain,
// follow the class; a domain without an entry stops at this static_assert.
template <ir::Domain D> struct EmitterOf {
    static_assert(domain_without_emitter<D>,"every instruction domain needs an entry in the domain table EmitterOf");
};

// Base of every entry of the domain table.
template <auto Member> struct EmittedBy {
    static constexpr auto member=Member;
};

struct FunctionEmitter {
    // The function and the module state it shares.
    const ir::Function& fn_;
    const std::unordered_map<std::string,const ir::Function*>& callable_functions_;
    const std::unordered_map<std::string,std::string>& external_symbols_;
    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
    const ArrayLayoutPolicy& array_layout_;
    const std::unordered_set<std::string>& recursive_callees_;
    StringPool& pool_;
    // How the function guards its call depth.
    bool guard_stack_depth_{};
    bool self_depth_recursive_{};
    // Its text and its per-function state.
    FunctionText function_text_;
    LlvmBuilder builder_{function_text_};
    SymbolTable symbols_;
    ScratchPlanner scratch_;
    TemporaryNames names_;
    StatementAttribution attribution_;
    DebugEmitter debug_;
    // The services it emits through.
    UnionBox union_box_;
    LifetimeEmitter lifetime_;
    FailFastEmitter fail_fast_;
    ErrorResultEmitter errors_;
    ReplValuePrinter repl_printer_;
    SymbolResolver resolver_;
    SelfDepthGuard self_depth_;
    // The emitters of the other domains, in the order of ir::Domain (debug_
    // and lifetime_ above emit the debug and lifetime domains).
    ConstantEmitter constant_{pool_,builder_,symbols_};
    NumericEmitter numeric_{pool_,builder_,symbols_,names_,fail_fast_,attribution_};
    ConversionEmitter conversion_{builder_,symbols_,scratch_,names_,union_box_,fail_fast_,attribution_};
    MemoryEmitter memory_{layouts_,array_layout_,builder_,symbols_,names_,debug_,lifetime_,attribution_};
    AggregateEmitter aggregate_{layouts_,array_layout_,builder_,symbols_,names_,lifetime_,union_box_,attribution_};
    BinEmitter bin_{builder_,symbols_,attribution_};
    TextEmitter text_{builder_,symbols_,scratch_,names_,union_box_,attribution_};
    TensorEmitter tensor_{builder_,symbols_,scratch_,names_,union_box_,attribution_};
    AutogradEmitter autograd_{builder_,symbols_,scratch_,names_,attribution_};
    ConsoleEmitter console_{builder_,symbols_,scratch_,names_,errors_,union_box_};
    FileEmitter file_{builder_,symbols_,scratch_,names_,errors_,union_box_};
    SystemEmitter system_{builder_,symbols_,names_,fail_fast_,union_box_};
    JsonEmitter json_{builder_,symbols_,names_,union_box_};
    HttpEmitter http_{builder_,symbols_,names_,union_box_};
    ConcurrencyEmitter concurrency_{builder_,symbols_,names_,attribution_};
    RandomEmitter random_{builder_,symbols_,names_,fail_fast_};
    CallEmitter call_{fn_,callable_functions_,external_symbols_,recursive_callees_,self_depth_recursive_,builder_,symbols_,names_,resolver_,attribution_};
    CheckEmitter check_{pool_,builder_,symbols_,names_,fail_fast_,attribution_};
    ControlFlowEmitter control_flow_{fn_,guard_stack_depth_,builder_,symbols_,lifetime_,attribution_};
    ReplEmitter repl_{pool_,builder_,symbols_,scratch_,repl_printer_};

    FunctionEmitter(const ir::Function& f,const ModuleContext& context,StringPool&p,bool guard,bool self_depth,
                    DebugModule* debug_module=nullptr,
                    std::optional<std::size_t> debug_id=std::nullopt,
                    std::size_t debug_file_id=0)
        :fn_(f),callable_functions_(context.callable_functions),external_symbols_(context.external_symbols),
         layouts_(context.layouts),array_layout_(context.array_layout),
         recursive_callees_(context.recursive_functions),pool_(p),
         guard_stack_depth_(guard),self_depth_recursive_(self_depth),
         attribution_(f,context,builder_,names_,pool_),
         debug_(builder_,symbols_,attribution_,debug_module,debug_id,debug_file_id),
         union_box_(builder_),lifetime_(builder_,names_,symbols_,layouts_),fail_fast_(builder_,names_,attribution_),
         errors_(builder_,names_,symbols_,union_box_),
         repl_printer_(builder_,names_,pool_,layouts_,array_layout_,union_box_),
         resolver_(fn_,external_symbols_,self_depth_recursive_),self_depth_(builder_,fn_,symbols_,self_depth_recursive_){}

    // The emitter of domain D, from the domain table.
    template <ir::Domain D> auto& emitter_for(){return this->*EmitterOf<D>::member;}

    // The function's pre-pass (FunctionPrepass): each instruction's domain
    // emitter reserves what the instruction needs.
    void prepass();

    void emit_instruction(const ir::Instruction& ins);

    // The steps of emit(), in their order.
    void emit_external_declaration();
    void emit_definition_header();
    void emit_entry_frame();
    void emit_missing_terminator(const ir::Block& b);
    void emit_blocks();

    std::string emit();
};

// The entries of the domain table, in the order of ir::Domain.
template <> struct EmitterOf<ir::Domain::debug> : EmittedBy<&FunctionEmitter::debug_> {};
template <> struct EmitterOf<ir::Domain::constant> : EmittedBy<&FunctionEmitter::constant_> {};
template <> struct EmitterOf<ir::Domain::numeric> : EmittedBy<&FunctionEmitter::numeric_> {};
template <> struct EmitterOf<ir::Domain::conversion> : EmittedBy<&FunctionEmitter::conversion_> {};
template <> struct EmitterOf<ir::Domain::memory> : EmittedBy<&FunctionEmitter::memory_> {};
template <> struct EmitterOf<ir::Domain::lifetime> : EmittedBy<&FunctionEmitter::lifetime_> {};
template <> struct EmitterOf<ir::Domain::aggregate> : EmittedBy<&FunctionEmitter::aggregate_> {};
template <> struct EmitterOf<ir::Domain::bin> : EmittedBy<&FunctionEmitter::bin_> {};
template <> struct EmitterOf<ir::Domain::text> : EmittedBy<&FunctionEmitter::text_> {};
template <> struct EmitterOf<ir::Domain::tensor> : EmittedBy<&FunctionEmitter::tensor_> {};
template <> struct EmitterOf<ir::Domain::autograd> : EmittedBy<&FunctionEmitter::autograd_> {};
template <> struct EmitterOf<ir::Domain::console> : EmittedBy<&FunctionEmitter::console_> {};
template <> struct EmitterOf<ir::Domain::file> : EmittedBy<&FunctionEmitter::file_> {};
template <> struct EmitterOf<ir::Domain::system> : EmittedBy<&FunctionEmitter::system_> {};
template <> struct EmitterOf<ir::Domain::json> : EmittedBy<&FunctionEmitter::json_> {};
template <> struct EmitterOf<ir::Domain::http> : EmittedBy<&FunctionEmitter::http_> {};
template <> struct EmitterOf<ir::Domain::concurrency> : EmittedBy<&FunctionEmitter::concurrency_> {};
template <> struct EmitterOf<ir::Domain::random> : EmittedBy<&FunctionEmitter::random_> {};
template <> struct EmitterOf<ir::Domain::call> : EmittedBy<&FunctionEmitter::call_> {};
template <> struct EmitterOf<ir::Domain::check> : EmittedBy<&FunctionEmitter::check_> {};
template <> struct EmitterOf<ir::Domain::control_flow> : EmittedBy<&FunctionEmitter::control_flow_> {};
template <> struct EmitterOf<ir::Domain::repl> : EmittedBy<&FunctionEmitter::repl_> {};

// Every domain below ir::domain_count has an entry: the table's static_assert
// stops an enumerator without one.
template <class> inline constexpr bool every_domain_has_emitter = false;
template <std::size_t... I>
inline constexpr bool every_domain_has_emitter<std::index_sequence<I...>> =
    (std::is_member_object_pointer_v<decltype(EmitterOf<static_cast<ir::Domain>(I)>::member)> && ...);
static_assert(every_domain_has_emitter<std::make_index_sequence<ir::domain_count>>,
              "every instruction domain needs an entry in the domain table EmitterOf");

void FunctionEmitter::prepass(){
    FunctionPrepass{fn_,symbols_}.run([&](const auto& n,[[maybe_unused]] const auto& instruction){
        using T=std::decay_t<decltype(n)>;
        constexpr auto domain=ir::InstructionTraits<T>::domain;
        if constexpr(reserves_before_emission<std::remove_reference_t<decltype(emitter_for<domain>())>,T>)
            emitter_for<domain>().reserve(n,instruction);
    });
}

void FunctionEmitter::emit_instruction(const ir::Instruction& ins){
    // A source location opens the location of the instructions that follow;
    // it is written before any segment, so it never belongs to one.
    if(const auto* location=std::get_if<ir::SourceLocation>(&ins)){debug_.emit(*location,ins);return;}
    const auto debug_begin=function_text_.offset();
    std::visit([&](const auto&n){
        emitter_for<ir::InstructionTraits<std::decay_t<decltype(n)>>::domain>().emit(n,ins);
    },ins);
    const auto debug_end=function_text_.offset();
    debug_.attach_pending_location(debug_begin,debug_end);
}

void FunctionEmitter::emit_external_declaration(){
    LlvmFunction declaration(llvm_type(fn_.result),"@"+*fn_.external_symbol);
    declaration.result_attributes(c_abi_return_attribute(fn_.result));
    for(const auto& parameter:fn_.parameters){
        declaration.parameter(llvm_type(parameter.type),c_abi_parameter_attribute(parameter.type,parameter.is_const),"");
        // A string or a bin reaches C with its length in bytes.
        if(parameter.type.kind==TypeKind::String||parameter.type.kind==TypeKind::Bin)
            declaration.parameter(i64,"","");
    }
    builder_.declare(declaration);
}

// The methods of the standard Map and Set collections are inlined into their
// callers (member_function_name::method names a method's function).
bool is_inlined_standard_collection_method(
    const ir::Function& fn,const std::unordered_map<std::string,ir::ClassLayout>& layouts){
    using member_function_name::method_prefix;
    const std::string_view name=fn.name;
    if(!name.starts_with(method_prefix)) return false;
    const auto member=name.substr(method_prefix.size());
    const auto dot=member.rfind('.');
    if(dot==std::string_view::npos) return false;
    const std::string owner(member.substr(0,dot));
    const auto layout=layouts.find(owner);
    if(layout==layouts.end()) return false;
    const bool standard=layout->second.standard_library;
    return standard_class::is_map_instance(owner,standard) ||
           standard_class::is_set_instance(owner,standard);
}

void FunctionEmitter::emit_definition_header(){
    LlvmFunction definition(llvm_type(fn_.result),"@"+resolver_.definition_symbol());
    if(fn_.entrypoint){
        definition.parameter(i32,"","%quidra.argc").parameter(ptr,"","%quidra.argv");
    }else{
        for(const auto& parameter:fn_.parameters){
            if(parameter.writable)
                definition.parameter(ptr,parameter.is_const?" nocapture nonnull readonly":" nocapture nonnull",
                                     symbols_.arg(parameter.name));
            else definition.parameter(llvm_type(parameter.type),"",symbols_.arg(parameter.name));
        }
    }
    if(self_depth_recursive_) definition.parameter(i64,"","%quidra.depth");
    if(is_inlined_standard_collection_method(fn_,layouts_)) definition.attribute("alwaysinline");
    if(const auto subprogram=debug_.subprogram_id()) definition.debug_subprogram(*subprogram);
    builder_.define(definition);
}

void FunctionEmitter::emit_entry_frame(){
    // The entry registers the module's source table before its first
    // statement, so every failure can name its file.
    if(fn_.entrypoint)builder_.call(runtime_abi::program::register_sources,{{ptr,LlvmOperand::global(source_table_symbol)}});
    if(fn_.entrypoint)builder_.call(runtime_abi::program::set_args,{{i32,"%quidra.argc"},{ptr,"%quidra.argv"}});
    if(guard_stack_depth_)builder_.call(runtime_abi::prelude::stack_enter,{});
    for(const auto&[name,type]:symbols_.locals())
        if(!symbols_.is_writable_parameter(name)){
            builder_.alloca_slot(symbols_.local(name),llvm_type(type),Align::none);
            if(type.kind==TypeKind::Real)builder_.store({exact_real_type,exact_real_none},symbols_.local(name),Align::none);
            else if(is_bare_integer(type))builder_.store({bare_integer_type,0},symbols_.local(name),Align::none);
            else if(requires_lifetime_management(type))builder_.store({ptr,"null"},symbols_.local(name),Align::none);
        }
    for(const auto&[name,type]:symbols_.references()){
        builder_.alloca_slot(symbols_.local(name),ptr,Align::none);
        builder_.store({ptr,"null"},symbols_.local(name),Align::none);
    }
    scratch_.emit_allocas(builder_);
    for(const auto&p:fn_.parameters)
        if(!p.writable)builder_.store({llvm_type(p.type),symbols_.arg(p.name)},symbols_.local(p.name),Align::none);
    for(std::size_t pi=0;pi<fn_.parameters.size();++pi){
        const auto&p=fn_.parameters[pi];
        debug_.declare_variable(p.name,p.name,p.type,fn_.source_line,pi+1);
    }
}

// The backend's rule for a block that needs no "unreachable" after its last
// instruction: a terminator, a call that never returns, or an indirect call
// with a Never result. The lowering has a rule of its own, which does not
// count the indirect call.
bool block_ends_in_backend(const ir::Block& b){
    bool term=false;
    if(!b.instructions.empty()){
        const auto&last=b.instructions.back();
        term=std::holds_alternative<ir::Return>(last)||std::holds_alternative<ir::ReturnVoid>(last)||
             std::holds_alternative<ir::Exit>(last)||std::holds_alternative<ir::FailError>(last)||
             std::holds_alternative<ir::Jump>(last)||std::holds_alternative<ir::Branch>(last)||
             (std::holds_alternative<ir::Call>(last)&&(std::get<ir::Call>(last).result.kind==TypeKind::Never||
                                                      std::get<ir::Call>(last).no_normal_return))||
             (std::holds_alternative<ir::IndirectCall>(last)&&
              std::get<ir::IndirectCall>(last).result.kind==TypeKind::Never);
    }
    return term;
}

void FunctionEmitter::emit_missing_terminator(const ir::Block& b){
    if(!block_ends_in_backend(b))builder_.unreachable();
}

void FunctionEmitter::emit_blocks(){
    for(std::size_t bi=0;bi<fn_.blocks.size();++bi){
        const auto&b=fn_.blocks[bi];
        builder_.block(b.label);
        if(bi==0) emit_entry_frame();
        for(const auto&i:b.instructions)emit_instruction(i);
        emit_missing_terminator(b);
    }
}

std::string FunctionEmitter::emit(){
    if(fn_.external_symbol){
        emit_external_declaration();
        return function_text_.str();
    }
    prepass();
    emit_definition_header();
    builder_.observe_calls(&attribution_);
    emit_blocks();
    builder_.observe_calls(nullptr);
    builder_.end_function();
    self_depth_.emit_wrapper();
    auto text=function_text_.str();
    debug_.attach_locations(text);
    return text;
}

} // namespace

std::string emit_function(const ir::Function& function,
                          const ModuleContext& context,
                          StringPool& pool,
                          bool guard_stack_depth,
                          bool self_depth_recursive,
                          DebugModule* debug_module,
                          std::optional<std::size_t> debug_subprogram,
                          std::size_t debug_file) {
    return FunctionEmitter{
        function, context, pool, guard_stack_depth, self_depth_recursive, debug_module,
        debug_subprogram, debug_file}.emit();
}

} // namespace quidra::llvm_backend
