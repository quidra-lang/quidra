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
#include "llvm_backend/call_acceptance_emitter.hpp"
#include "llvm_backend/node_value_ownership.hpp"
#include "llvm_backend/node_owned_type.hpp"
#include "llvm_backend/check_emitter.hpp"
#include "llvm_backend/concurrency_emitter.hpp"
#include "llvm_backend/parallel_node_emission.hpp"
#include "llvm_backend/console_emitter.hpp"
#include "llvm_backend/constant_emitter.hpp"
#include "llvm_backend/control_flow_emitter.hpp"
#include "llvm_backend/conversion_emitter.hpp"
#include "llvm_backend/debug_emitter.hpp"
#include "llvm_backend/error_result_emitter.hpp"
#include "llvm_backend/error_value_emitter.hpp"
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
#include "llvm_backend/tail_record_emitter.hpp"
#include "llvm_backend/tail_continuations.hpp"
#include "llvm_backend/text_emitter.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"

#include <algorithm>
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
    InlineScopePlan inline_scopes_{fn_};
    const ir::analysis::TailCalls& tail_calls_;
    CallSiteMetadata* call_site_metadata_{};
    const std::unordered_map<std::string,const ir::Function*>& callable_functions_;
    const std::unordered_map<std::string,std::string>& external_symbols_;
    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
    const ArrayLayoutPolicy& array_layout_;
    const std::unordered_set<std::string>& recursive_callees_;
    StringPool& pool_;
    // How the function guards its call depth.
    bool node_cleanup_{};
    bool guard_stack_depth_{};
    bool self_depth_recursive_{};
    // Its text and its per-function state.
    FunctionText function_text_;
    LlvmBuilder builder_{function_text_};
    SymbolTable symbols_;
    ScratchPlanner scratch_;
    TemporaryNames names_;
    std::optional<NodeValueOwnership> node_values_;
    CallAcceptanceEmitter call_acceptance_{builder_,scratch_,names_};
    StatementAttribution attribution_;
    DebugEmitter debug_;
    // The constant error records of the function's error sites.
    ErrorRecords records_{builder_,pool_,attribution_};
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
    NumericEmitter numeric_{pool_,builder_,symbols_,names_,fail_fast_,attribution_,scratch_,records_,layouts_};
    ConversionEmitter conversion_{builder_,symbols_,scratch_,names_,union_box_,fail_fast_,attribution_,records_};
    MemoryEmitter memory_{layouts_,array_layout_,builder_,symbols_,names_,debug_,lifetime_,attribution_};
    AggregateEmitter aggregate_{layouts_,array_layout_,builder_,symbols_,names_,lifetime_,union_box_,attribution_};
    BinEmitter bin_{builder_,symbols_,attribution_};
    TextEmitter text_{pool_,builder_,symbols_,scratch_,names_,union_box_,attribution_,records_};
    TensorEmitter tensor_{builder_,symbols_,scratch_,names_,union_box_,attribution_,records_};
    AutogradEmitter autograd_{builder_,symbols_,scratch_,names_,attribution_};
    ConsoleEmitter console_{builder_,symbols_,scratch_,names_,errors_,union_box_,records_};
    FileEmitter file_{builder_,symbols_,scratch_,names_,errors_,union_box_,records_};
    SystemEmitter system_{builder_,symbols_,names_,fail_fast_,union_box_};
    JsonEmitter json_{builder_,symbols_,names_,union_box_,records_};
    HttpEmitter http_{builder_,symbols_,names_,union_box_,records_};
    ConcurrencyEmitter concurrency_{builder_,symbols_,names_,attribution_,scratch_,union_box_};
    RandomEmitter random_{builder_,symbols_};
    CallEmitter call_{fn_,callable_functions_,external_symbols_,recursive_callees_,self_depth_recursive_,builder_,symbols_,names_,resolver_,attribution_,pool_,scratch_};
    CheckEmitter check_{pool_,builder_,symbols_,names_,fail_fast_,attribution_};
    ControlFlowEmitter control_flow_{fn_,guard_stack_depth_,builder_,symbols_,lifetime_,attribution_};
    ErrorValueEmitter error_{pool_,builder_,symbols_,scratch_,union_box_,records_};
    ReplEmitter repl_{pool_,builder_,symbols_,scratch_,repl_printer_};
    // The call sites of a native artifact's function (none otherwise).
    std::optional<FunctionCallSites> call_sites_;
    std::optional<TailRecordEmitter> tail_records_;
    TailContinuations tail_continuations_;
    bool emitting_tail_suffix_{};
    std::optional<std::size_t> call_site_subprogram_;

    FunctionEmitter(const ir::Function& f,const ModuleContext& context,StringPool&p,bool guard,bool self_depth,
                    DebugModule* debug_module=nullptr,
                    std::optional<std::size_t> debug_id=std::nullopt,
                    std::size_t debug_file_id=0,
                    CallSiteMetadata* call_site_metadata=nullptr,
                    std::optional<std::uint32_t> call_site_function=std::nullopt)
        :fn_(f),tail_calls_(context.tail_calls),call_site_metadata_(call_site_metadata),
         callable_functions_(context.callable_functions),external_symbols_(context.external_symbols),
         layouts_(context.layouts),array_layout_(context.array_layout),
         recursive_callees_(context.recursive_functions),pool_(p),
         node_cleanup_(context.node_cleanup_functions.contains(f.name)),
         guard_stack_depth_(guard),self_depth_recursive_(self_depth),
         attribution_(f,context,builder_,names_,pool_),
         debug_(builder_,symbols_,attribution_,debug_module,debug_id,debug_file_id),
         union_box_(builder_,&symbols_),lifetime_(builder_,names_,symbols_,layouts_),fail_fast_(builder_,names_,attribution_),
         errors_(builder_,names_,symbols_,union_box_,records_),
         repl_printer_(builder_,names_,pool_,layouts_,array_layout_,union_box_),
         resolver_(fn_,external_symbols_,self_depth_recursive_),self_depth_(builder_,fn_,symbols_,self_depth_recursive_){
        symbols_.inline_scopes(inline_scopes_);
        if(call_site_metadata&&call_site_function){
            // Release: the function's own subprogram; --debug: DebugModule's,
            // in which each call's location is a block of the call's site.
            const bool release=call_site_metadata->mode()==CallSiteMetadata::Mode::release;
            const auto scope=release?call_site_metadata->subprogram(*call_site_function):*debug_id;
            if(release) call_site_subprogram_=scope;
            else if(const auto location=debug_module->location_of_subprogram(*debug_id))
                debug_.locate_declarations(*location);
            call_sites_.emplace(*call_site_metadata,context,&attribution_,*call_site_function,scope,
                                debug_file_id,f.source_line);
            const bool recorded = std::any_of(tail_calls_.sites.begin(), tail_calls_.sites.end(),
                [&](const auto& entry) { return entry.second.owner == f.name && entry.second.recorded; });
            if (recorded) tail_records_.emplace(builder_, names_, self_depth_recursive_,
                                                call_site_metadata->next_site());
        }
    }

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
    void emit_inline_begin(std::uint64_t scope);
    void emit_inline_end(std::uint64_t scope);
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
template <> struct EmitterOf<ir::Domain::error> : EmittedBy<&FunctionEmitter::error_> {};
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
    if(fn_.parallel_node)for(const auto& slot:fn_.parallel_node->reference_slots)symbols_.capture_reference_slot(slot);
    FunctionPrepass{fn_,symbols_}.run([&](const auto& n,[[maybe_unused]] const auto& instruction){
        using T=std::decay_t<decltype(n)>;
        constexpr auto domain=ir::InstructionTraits<T>::domain;
        if constexpr(reserves_before_emission<std::remove_reference_t<decltype(emitter_for<domain>())>,T>)
            emitter_for<domain>().reserve(n,instruction);
    });
    if(node_values_) {
        symbols_.select_scope(0);
        const auto reserve_call=[&](const ir::Instruction& instruction) {
            if(const auto* call=std::get_if<ir::Call>(&instruction))call_.reserve(*call,instruction);
            else if(const auto* call=std::get_if<ir::IndirectCall>(&instruction))call_.reserve(*call,instruction);
        };
        for(const auto& [_,instruction]:tail_continuations_.edges)reserve_call(instruction);
        for(const auto& block:tail_continuations_.blocks)
            for(const auto& instruction:block.instructions)reserve_call(instruction);
    }
}

void FunctionEmitter::emit_instruction(const ir::Instruction& ins){
    // A source location opens the location of the instructions that follow;
    // it is written before any segment, so it never belongs to one.
    if(std::holds_alternative<ir::StatementBoundary>(ins)) return;
    if(const auto* location=std::get_if<ir::SourceLocation>(&ins)){debug_.emit(*location,ins);return;}
    if (!emitting_tail_suffix_) {
        const auto edge = tail_continuations_.edges.find(&ins);
        if (edge != tail_continuations_.edges.end()) {
            const bool previous = emitting_tail_suffix_;
            emitting_tail_suffix_ = true;
            emit_instruction(edge->second);
            emitting_tail_suffix_ = previous;
            return;
        }
    }
    const auto debug_begin=function_text_.offset();
    if (call_sites_) {
        const auto* call = std::get_if<ir::Call>(&ins);
        call_sites_->set_synthesized(call && call->shape_forwarder);
        const auto found = tail_calls_.sites.find(&ins);
        call_sites_->set_tail_site(found == tail_calls_.sites.end() ? nullptr : &found->second,
                                   tail_records_ ? &*tail_records_ : nullptr);
    }
    if (!emitting_tail_suffix_ && tail_records_ && tail_calls_.self_sites.contains(fn_.name) &&
        (std::holds_alternative<ir::Return>(ins) || std::holds_alternative<ir::ReturnVoid>(ins)) &&
        !tail_calls_.tail_returns.contains(&ins)) tail_records_->normal_return();
    std::visit([&](const auto&n){
        if(call_sites_){
            // The position of the instruction's calls in the call-site
            // table: its own where it has one.
            if constexpr(requires{n.position.line;n.position.column;})
                call_sites_->set_instruction_position(static_cast<std::uint32_t>(n.position.line),
                                                      static_cast<std::uint32_t>(n.position.column));
            else call_sites_->set_instruction_position(0,0);
        }
        using Node=std::decay_t<decltype(n)>;
        if constexpr(std::is_same_v<Node,ir::InlineScopeBegin>)emit_inline_begin(n.scope);
        else if constexpr(std::is_same_v<Node,ir::InlineScopeCleanup>) {
            (void)inline_scopes_.at(n.scope);
            if(symbols_.selected_scope()!=n.scope)throw std::logic_error("inline cleanup leaves its declared scope");
            lifetime_.cleanup_scope(n.scope);
        }
        else if constexpr(std::is_same_v<Node,ir::InlineScopeEnd>)emit_inline_end(n.scope);
        else if constexpr(std::is_same_v<Node,ir::InlineBindAddress>) {
            if(!symbols_.storage_scope(n.name) || symbols_.storage_scope(n.name)!=symbols_.selected_scope() ||
               !symbols_.is_writable_parameter(n.name))throw std::logic_error("inline raw address lacks an explicit ABI slot");
            builder_.getelementptr(symbols_.arg(n.name),Inbounds::no,i8,symbols_.value(n.address),{{i64,0}});
        } else if constexpr(std::is_same_v<Node,ir::InlineExit>) {
            (void)inline_scopes_.at(n.scope);
            if(symbols_.selected_scope()!=n.scope)throw std::logic_error("inline exit leaves its declared scope");
            const auto status=names_.value("inline.exit.status");
            builder_.cast(status,CastOp::trunc,{i64,symbols_.value(n.status)},i32);
            builder_.call(runtime_abi::program::process_exit,{{i32,status}});
            builder_.unreachable();
        } else emitter_for<ir::InstructionTraits<Node>::domain>().emit(n,ins);
        if(node_values_) {
            // These operations return one owning result and do not perform
            // another fallible operation after publishing that SSA value.
            if constexpr(std::is_same_v<Node,ir::Unary> || std::is_same_v<Node,ir::Binary> ||
                         std::is_same_v<Node,ir::ComplexMake>)
                symbols_.acquire_owned(n.out,symbols_.type_at(n.out),symbols_.value(n.out));
            node_values_->certify_result(ins);
        }
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
    const bool indirect=tuple_indirect_result(fn_.result);
    LlvmFunction definition(indirect?void_type:llvm_type(fn_.result),"@"+resolver_.definition_symbol());
    if(indirect) definition.parameter(ptr," noalias "+tuple_result_attribute(fn_.result),"%quidra.result");
    if(fn_.entrypoint){
        definition.parameter(i32,"","%quidra.argc").parameter(ptr,"","%quidra.argv");
    }else{
        for(const auto& parameter:fn_.parameters){
            if(parameter.writable)
                definition.parameter(ptr,parameter.target_escapes?(parameter.is_const?" nonnull readonly":" nonnull"):(parameter.is_const?" nocapture nonnull readonly":" nocapture nonnull"),
                                     symbols_.arg(parameter.name));
            else definition.parameter(llvm_type(parameter.type),"",symbols_.arg(parameter.name));
        }
    }
    if(self_depth_recursive_) definition.parameter(i64,"","%quidra.depth");
    if(fn_.shape_wrapper || is_inlined_standard_collection_method(fn_,layouts_))
        definition.attribute("alwaysinline");
    if(call_site_subprogram_) definition.debug_subprogram(*call_site_subprogram_);
    else if(const auto subprogram=debug_.subprogram_id()) definition.debug_subprogram(*subprogram);
    builder_.define(definition);
}

void FunctionEmitter::emit_entry_frame(){
    // The entry registers the module's source table before its first
    // statement, so every failure can name its file.
    if(fn_.entrypoint)builder_.call(runtime_abi::program::register_sources,{{ptr,LlvmOperand::global(source_table_symbol)}});
    if(fn_.entrypoint)builder_.call(runtime_abi::program::set_args,{{i32,"%quidra.argc"},{ptr,"%quidra.argv"}});

    if(guard_stack_depth_ && !symbols_.node_cleanup())
        builder_.call(runtime_abi::prelude::stack_enter,{});
    if(symbols_.node_cleanup()) {
        builder_.call("%quidra.node.frame",runtime_abi::program::parallel_frame_create,
            {{i64,symbols_.cleanup_bytes()},{i64,8},
             {ptr,LlvmOperand::global(symbols_.cleanup_symbol())}});
        builder_.icmp("%quidra.node.frame.active",IntPredicate::ne,ptr,"%quidra.node.frame","null");
    }
    const auto owned_slot=[&](const std::string& name,LlvmType type) {
        if(const auto* slot=symbols_.cleanup_slot(name)) {
            const auto heap="%quidra.node.slot."+llvm_text::sanitize_identifier(name);
            const auto stack=symbols_.local(name)+".stack";
            builder_.alloca_slot(stack,type,Align::none);
            builder_.getelementptr(heap,Inbounds::no,i8,"%quidra.node.frame",{{i64,slot->offset}});
            builder_.select(symbols_.local(name),"%quidra.node.frame.active",{ptr,heap},{ptr,stack});
        } else builder_.alloca_slot(symbols_.local(name),type,Align::none);
    };
    for(const auto&[name,type]:symbols_.locals())
        if(!symbols_.storage_scope(name) && !symbols_.is_writable_parameter(name)){
            owned_slot(name,llvm_type(type));
            if(type.kind==TypeKind::Tuple)builder_.store({llvm_type(type),"zeroinitializer"},symbols_.local(name),Align::none);
            else if(type.kind==TypeKind::Real)builder_.store({exact_real_type,exact_real_none},symbols_.local(name),Align::none);
            else if(is_bare_integer(type))builder_.store({bare_integer_type,0},symbols_.local(name),Align::none);
            else if(requires_lifetime_management(type))builder_.store({ptr,"null"},symbols_.local(name),Align::none);
        }
    for(const auto&[name,type]:symbols_.references()){
        if(symbols_.storage_scope(name))continue;
        owned_slot(name,ptr);
        builder_.store({ptr,"null"},symbols_.local(name),Align::none);
    }
    for(const auto& [name,type]:symbols_.locals())if(symbols_.storage_scope(name) && !symbols_.is_writable_parameter(name))
        builder_.alloca_slot(symbols_.local(name)+(symbols_.cleanup_slot(name)?".stack":""),llvm_type(type),Align::none);
    for(const auto& [name,type]:symbols_.references())if(symbols_.storage_scope(name)) {
        (void)type;builder_.alloca_slot(symbols_.local(name)+(symbols_.cleanup_slot(name)?".stack":""),ptr,Align::none);
    }
    scratch_.emit_allocas(builder_);
    if(node_values_)function_text_.append(NodeValueOwnership::entry_marker);
    for(const auto&p:fn_.parameters)
        if(!p.writable)builder_.store({llvm_type(p.type),symbols_.arg(p.name)},symbols_.local(p.name),Align::none);
    if(node_values_) {
        bool consumes=false;
        for(const auto& p:fn_.parameters)
            if(!p.writable && !p.borrowed && node_owns_value(p.type))consumes=true;
        if(consumes)call_acceptance_.accept(resolver_.call_symbol(fn_.name));
    }
    if(guard_stack_depth_ && symbols_.node_cleanup())
        builder_.call(runtime_abi::prelude::stack_enter,{});
    for(std::size_t pi=0;pi<fn_.parameters.size();++pi){
        const auto&p=fn_.parameters[pi];
        debug_.declare_variable(p.name,p.name,p.type,fn_.source_line,pi+1);
    }
}

void FunctionEmitter::emit_inline_begin(std::uint64_t identity) {
    const auto& scope=inline_scopes_.at(identity);
    if(symbols_.selected_scope()!=identity)throw std::logic_error("inline Begin lacks its declared block ownership");
    attribution_.begin_inline_scope(scope);
    if(symbols_.node_cleanup(identity)) {
        builder_.call(symbols_.frame(identity),runtime_abi::program::parallel_frame_create,
            {{i64,symbols_.cleanup_bytes(identity)},{i64,8},{ptr,LlvmOperand::global(symbols_.cleanup_symbol(identity))}});
        builder_.icmp(symbols_.frame_active(identity),IntPredicate::ne,ptr,symbols_.frame(identity),"null");
    }
    const auto storage=[&](const std::string& name) {
        if(const auto* slot=symbols_.cleanup_slot(name)) {
            const auto heap="%quidra.node.slot."+llvm_text::sanitize_identifier(name);
            builder_.getelementptr(heap,Inbounds::no,i8,symbols_.frame(identity),{{i64,slot->offset}});
            builder_.select(symbols_.local(name),symbols_.frame_active(identity),
                {ptr,heap},{ptr,symbols_.local(name)+".stack"});
        }
    };
    for(const auto& [name,type]:symbols_.locals()) {
        if(symbols_.storage_scope(name)!=identity || symbols_.is_writable_parameter(name))continue;
        storage(name);
        if(type.kind==TypeKind::Tuple)builder_.store({llvm_type(type),"zeroinitializer"},symbols_.local(name),Align::none);
        else if(type.kind==TypeKind::Real)builder_.store({exact_real_type,exact_real_none},symbols_.local(name),Align::none);
        else if(is_bare_integer(type))builder_.store({bare_integer_type,0},symbols_.local(name),Align::none);
        else if(requires_lifetime_management(type))builder_.store({ptr,"null"},symbols_.local(name),Align::none);
    }
    for(const auto& [name,type]:symbols_.references())if(symbols_.storage_scope(name)==identity) {
        (void)type;storage(name);builder_.store({ptr,"null"},symbols_.local(name),Align::none);
    }
    if(node_values_)function_text_.append(NodeValueOwnership::scope_entry_marker(identity));
}
void FunctionEmitter::emit_inline_end(std::uint64_t identity) {
    if(symbols_.selected_scope()!=identity)throw std::logic_error("inline End leaves its declared scope");
    attribution_.end_inline_scope(inline_scopes_.at(identity));
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
             std::holds_alternative<ir::Exit>(last)||std::holds_alternative<ir::InlineExit>(last)||std::holds_alternative<ir::FailError>(last)||
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
        symbols_.select_block(b.label);
        const auto scope=inline_scopes_.block_scope(b.label);
        attribution_.select_inline_scope(scope?&inline_scopes_.at(scope):nullptr);
        builder_.block(b.label);
        if(bi==0) emit_entry_frame();
        for(const auto&i:b.instructions)emit_instruction(i);
        emit_missing_terminator(b);
    }
    emitting_tail_suffix_ = true;
    symbols_.select_scope(0);
    attribution_.select_inline_scope(nullptr);
    for (const auto& block : tail_continuations_.blocks) {
        builder_.block(block.label);
        for (const auto& instruction : block.instructions) emit_instruction(instruction);
        emit_missing_terminator(block);
    }
    emitting_tail_suffix_ = false;
}

std::string FunctionEmitter::emit(){
    if(fn_.external_symbol){
        emit_external_declaration();
        return function_text_.str();
    }
    if (call_sites_) tail_continuations_ = tail_continuations(fn_, tail_calls_, names_);
    const auto scoped_cleanup=std::any_of(fn_.inline_scopes.begin(),fn_.inline_scopes.end(),
        [&](const auto& scope){return inline_scopes_.enabled(scope.identity,node_cleanup_);});
    if(node_cleanup_ || scoped_cleanup) {
        node_values_.emplace(fn_,symbols_,builder_,function_text_,names_,layouts_,inline_scopes_,node_cleanup_);
        symbols_.node_ownership(&*node_values_);
        call_.node_ownership(&*node_values_,&call_acceptance_);
    }
    prepass();
    if(node_cleanup_)symbols_.plan_node_cleanup(fn_.name);
    for(const auto& scope:fn_.inline_scopes)if(inline_scopes_.enabled(scope.identity,node_cleanup_))
        symbols_.plan_node_cleanup(fn_.name,scope.identity);
    emit_definition_header();
    builder_.observe_calls(&attribution_);
    if(call_sites_) builder_.locate_calls(&*call_sites_);
    emit_blocks();
    builder_.observe_calls(nullptr);
    builder_.locate_calls(nullptr);
    builder_.end_function();
    self_depth_.emit_wrapper();
    auto text=function_text_.str()+emit_parallel_node_wrapper(fn_,call_sites_?call_site_metadata_:nullptr);
    if (tail_records_) TailRecordEmitter::resolve_range(text, call_site_metadata_->next_site());
    debug_.attach_locations(text);
    if(node_values_) {
        const auto replace=[&](const std::string& marker,const std::string& replacement) {
            std::size_t offset=0;
            while((offset=text.find(marker,offset))!=std::string::npos) {
                text.replace(offset,marker.size(),replacement);offset+=replacement.size();
            }
        };
        replace(NodeValueOwnership::entry_marker,node_values_->finish_entry());
        replace(NodeValueOwnership::cleanup_marker,node_values_->finish_cleanup());
        for(const auto& scope:fn_.inline_scopes) {
            replace(NodeValueOwnership::scope_entry_marker(scope.identity),node_values_->finish_entry(scope.identity));
            replace(NodeValueOwnership::scope_cleanup_marker(scope.identity),node_values_->finish_cleanup(scope.identity));
        }
    }
    return text+lifetime_.node_cleanup_helpers()+
        (node_values_?node_values_->helpers():std::string{});
}

} // namespace

std::string emit_function(const ir::Function& function,
                          const ModuleContext& context,
                          StringPool& pool,
                          bool guard_stack_depth,
                          bool self_depth_recursive,
                          DebugModule* debug_module,
                          std::optional<std::size_t> debug_subprogram,
                          std::size_t debug_file,
                          CallSiteMetadata* call_sites,
                          std::optional<std::uint32_t> call_site_function) {
    return FunctionEmitter{
        function, context, pool, guard_stack_depth, self_depth_recursive, debug_module,
        debug_subprogram, debug_file, call_sites, call_site_function}.emit();
}

} // namespace quidra::llvm_backend
