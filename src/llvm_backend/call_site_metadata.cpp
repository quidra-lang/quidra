#include "llvm_backend/call_site_metadata.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/statement_attribution.hpp"
#include "llvm_backend/tail_record_emitter.hpp"
#include "llvm_text/escape.hpp"
#include "quidra/abi/symbols.hpp"
#include "quidra/member_function_names.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace quidra::llvm_backend {

namespace {

// The display name of a class: a generic instance's declaration name
// ("__quidra_gc_Box_<hash>" -> "Box"), without a module qualifier.
std::string class_display_name(std::string name) {
    const std::string generic(abi::symbol_namespace::internal_prefix);
    if (name.rfind(generic, 0) == 0) {
        const auto kind_end = name.find('_', generic.size());
        const auto hash = name.rfind('_');
        if (kind_end != std::string::npos && hash != std::string::npos && hash > kind_end)
            name = name.substr(kind_end + 1, hash - kind_end - 1);
    }
    if (const auto dot = name.rfind('.'); dot != std::string::npos) name = name.substr(dot + 1);
    return name;
}

} // namespace

std::string call_site_display_name(const ir::Function& function) {
    if (function.entrypoint) return "<top-level>";
    const std::string& name = function.source_function.empty() ? function.name : function.source_function;
    using member_function_name::constructor_prefix;
    using member_function_name::method_prefix;
    if (name.rfind(method_prefix, 0) == 0) {
        const auto rest = name.substr(method_prefix.size());
        const auto dot = rest.rfind('.');
        if (dot != std::string::npos)
            return class_display_name(rest.substr(0, dot)) + "." + rest.substr(dot + 1) + "()";
    }
    if (name.rfind(constructor_prefix, 0) == 0) {
        const auto rest = name.substr(constructor_prefix.size());
        const auto dot = rest.rfind('.');
        return class_display_name(dot == std::string::npos ? rest : rest.substr(0, dot)) + "()";
    }
    return class_display_name(name) + "()";
}

CallSiteMetadata::CallSiteMetadata(const ir::Module& module, const ModuleContext& context,
                                   Mode mode, bool library, DebugModule* debug)
    : mode_(mode), debug_(debug), function_ids_(module.functions.size()) {
    info_.library = library;
    info_.debug = mode == Mode::debug;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        const auto& f = module.functions[i];
        if (f.external_symbol) continue;
        if (mode_ == Mode::debug && (!debug_ || !debug_->subprogram_id(i))) continue;
        const auto id = static_cast<std::uint32_t>(info_.functions.size());
        function_ids_[i] = id;
        CallSiteFunction function;
        function.display = call_site_display_name(f);
        if (f.shape_wrapper)
            function.flags |= call_site_function_flags::transparent_forwarder;
        if (f.entrypoint) {
            function.kind = CallSiteFunctionKind::entry;
            function.symbols.emplace_back(abi::symbol_namespace::entry);
        } else {
            function.kind = f.parallel_node ? CallSiteFunctionKind::synthesized : context.user_source_index.contains(f.source_file)
                                ? CallSiteFunctionKind::user
                                : CallSiteFunctionKind::package;
            const auto symbol = abi::user_symbol(f.name);
            function.symbols.push_back(symbol);
            if (context.self_depth_recursive_functions.contains(f.name)) {
                function.symbols.push_back(symbol + ".depth");
                function.flags |= call_site_function_flags::self_depth;
            }
            if (context.tail_calls.self_sites.contains(f.name))
                function.flags |= call_site_function_flags::self_tail;
            if (context.address_taken_functions.contains(f.name))
                function.flags |= call_site_function_flags::address_taken;
        }
        for (const auto& symbol : function.symbols) symbols_.emplace(symbol, id);
        info_.functions.push_back(std::move(function));
    }
    subprograms_.assign(info_.functions.size(), 0);
    if (mode_ == Mode::debug)
        for (std::size_t i=0;i<function_ids_.size();++i)
            if (function_ids_[i]) subprograms_[*function_ids_[i]] = *debug_->subprogram_id(i);
}

std::uint32_t CallSiteMetadata::export_entry_id(std::size_t index, const std::string& symbol) {
    if (const auto found = export_ids_.find(index); found != export_ids_.end()) return found->second;
    const auto id = static_cast<std::uint32_t>(info_.functions.size());
    export_ids_.emplace(index, id);
    CallSiteFunction function;
    const auto exported = function_ids_[index];
    function.display = exported ? info_.functions[*exported].display : std::string("()");
    function.kind = CallSiteFunctionKind::synthesized;
    function.flags = call_site_function_flags::export_entry;
    function.symbols.push_back(symbol);
    info_.functions.push_back(std::move(function));
    subprograms_.push_back(0);
    return id;
}

std::size_t CallSiteMetadata::next_id() {
    if (mode_ == Mode::debug) return debug_->next_id();
    return next_release_id_++;
}

std::pair<std::size_t,std::size_t> CallSiteMetadata::parallel_entry(const ir::Function& function) {
    const auto body=symbol_function(abi::user_symbol(function.name));
    if(!body || !function.parallel_node)throw std::logic_error("parallel entry lacks a body function");
    const auto id=static_cast<std::uint32_t>(info_.functions.size());
    CallSiteFunction entry;
    entry.display="<node body>()";
    entry.kind=CallSiteFunctionKind::synthesized;
    entry.symbols.push_back(abi::user_symbol(function.parallel_node->wrapper));
    symbols_.emplace(entry.symbols.front(),id);
    info_.functions.push_back(std::move(entry));
    subprograms_.push_back(0);
    const auto scope=subprogram(id);
    const auto file=mode_==Mode::debug?debug_->source_file_id(function.source_file):0;
    if(mode_==Mode::debug)parallel_scopes_.push_back({id,file,function.source_line});
    CallSite site;
    site.owner=id;site.callee=*body;site.callee_kind=CallSiteCallee::function;
    site.flags=call_site_flags::synthesized;
    return {scope,locate(site,scope,file,function.source_line,function.source_column)};
}

std::size_t CallSiteMetadata::subprogram(std::uint32_t function) {
    auto& id = subprograms_.at(function);
    if (id == 0) id = next_id();
    return id;
}

std::optional<std::uint32_t> CallSiteMetadata::symbol_function(std::string_view symbol) const {
    const auto found = symbols_.find(std::string(symbol));
    if (found == symbols_.end()) return std::nullopt;
    return found->second;
}

std::size_t CallSiteMetadata::locate(const CallSite& site, std::size_t scope, std::size_t file,
                                     std::uint32_t line, std::uint32_t column, std::size_t inlined_at) {
    const auto id = static_cast<std::uint32_t>(info_.sites.size());
    info_.sites.push_back(site);
    Location location;
    location.scope = scope;
    location.site = id;
    location.inlined_at = inlined_at;
    if (mode_ == Mode::debug) {
        location.block = next_id();
        location.file = file;
        location.line = line;
        location.column = column;
    }
    location.id = next_id();
    locations_.push_back(location);
    return location.id;
}

void CallSiteMetadata::emit_anchor_reference(llvm_text::LlvmModule& module) const {
    if (!info_.library) return;
    std::string text = "@" + std::string(call_site_anchor_symbol) + " = external global i8\n";
    text += "@.quidra.calls.anchor = private constant ptr @" + std::string(call_site_anchor_symbol) +
            "\n";
    text += "@llvm.used = appending global [1 x ptr] [ptr @.quidra.calls.anchor], section "
            "\"llvm.metadata\"\n";
    module.append(llvm_text::LlvmModule::Section::constants, text);
}

void CallSiteMetadata::group_sites() {
    if (!site_remap_.empty()) return;
    if (info_.sites.size()>std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("call-site id overflow");
    std::vector<std::uint32_t> order;
    order.reserve(info_.sites.size()-1);
    for (std::size_t id=1;id<info_.sites.size();++id) {
        if (info_.sites[id].owner>=info_.functions.size())
            throw std::logic_error("call-site logical owner unavailable");
        order.push_back(static_cast<std::uint32_t>(id));
    }
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return info_.sites[a].owner<info_.sites[b].owner;});
    std::vector<CallSite> grouped;
    grouped.reserve(info_.sites.size());grouped.push_back(info_.sites[0]);
    site_remap_.resize(info_.sites.size());site_ranges_.resize(info_.functions.size());
    for (const auto old:order) {
        const auto id=static_cast<std::uint32_t>(grouped.size());
        const auto& site=info_.sites[old];auto& range=site_ranges_[site.owner];
        if (!range.second) range.first=id;
        ++range.second;site_remap_[old]=id;grouped.push_back(site);
    }
    for (auto& location:locations_) location.site=site_remap_.at(location.site);
    info_.sites=std::move(grouped);
}

std::string CallSiteMetadata::resolve_tail_operands(std::string text) const {
    const auto resolve=[&](std::string_view marker,const auto& lookup) {
        std::size_t line=0;
        while (line<text.size()) {
            auto end=text.find('\n',line);if(end==std::string::npos)end=text.size();
            // Never rewrite source-string constants or metadata payloads.
            const auto generated=std::string_view(text).substr(line,end-line);
            if (generated.starts_with("  call void @"+std::string(runtime_abi::tail::record.symbol())+"(") ||
                generated.starts_with("  call void @"+std::string(runtime_abi::tail::return_cleanup.symbol())+"(")) {
                auto at=text.find(marker,line);
                while (at!=std::string::npos && at<end) {
                    const auto begin=at+marker.size(),close=text.find("__",begin);
                    if(close==std::string::npos||close>end)throw std::logic_error("unterminated tail site operand");
                    std::uint32_t id{};
                    const auto parsed=std::from_chars(text.data()+begin,text.data()+close,id);
                    if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+close)throw std::logic_error("invalid tail site operand");
                    const auto value=std::to_string(lookup(id));
                    const auto bytes=close+2-at;text.replace(at,bytes,value);end=end-bytes+value.size();
                    at=text.find(marker,at+value.size());
                }
            }
            line=end==text.size()?end:end+1;
        }
    };
    resolve("__quidra_tail_site_",[&](auto id){return site_remap_.at(id);});
    resolve("__quidra_tail_begin_",[&](auto id){return site_ranges_.at(id).first;});
    resolve("__quidra_tail_end_",[&](auto id){const auto& range=site_ranges_.at(id);return range.first+range.second;});
    return text;
}

void CallSiteMetadata::emit_metadata(llvm_text::LlvmModule& module) {
    group_sites();
    std::ostringstream out;
    if (mode_ == Mode::release) {
        out << "\n!llvm.dbg.cu = !{!0}\n"
            << "!llvm.module.flags = !{!2, !3}\n"
            << "!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "
               "\"Quidra\", isOptimized: true, runtimeVersion: 0, emissionKind: "
               "LineTablesOnly)\n"
            << "!1 = !DIFile(filename: \"quidra-call-sites\", directory: \"\")\n"
            << "!2 = !{i32 2, !\"Dwarf Version\", i32 4}\n"
            << "!3 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
            << "!4 = !DISubroutineType(types: !5)\n"
            << "!5 = !{}\n";
        for (std::size_t function = 0; function < subprograms_.size(); ++function) {
            if (subprograms_[function] == 0) continue;
            // The definition's symbol: a self-depth function's body, not its
            // public wrapper.
            const auto& info = info_.functions[function];
            out << "!" << subprograms_[function] << " = distinct !DISubprogram(name: \""
                << llvm_text::escape_metadata(info.symbols.back())
                << "\", scope: !1, file: !1, type: !4, ";
            if (info.kind == CallSiteFunctionKind::synthesized) out << "flags: DIFlagArtificial, ";
            out << "spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)\n";
        }
        for (const auto& location : locations_) {
            out << "!" << location.id << " = !DILocation(line: " << location.site
                << ", scope: !" << location.scope;
            if (location.inlined_at) out << ", inlinedAt: !" << location.inlined_at;
            out << ")\n";
        }
    } else {
        for(const auto& scope:parallel_scopes_) {
            const auto& symbol=info_.functions[scope.function].symbols.front();
            out<<"!"<<subprograms_[scope.function]<<" = distinct !DISubprogram(name: \""
               <<llvm_text::escape_metadata(symbol)<<"\", scope: !"<<scope.file<<", file: !"<<scope.file
               <<", line: "<<std::max<std::uint32_t>(1,scope.line)
               <<", type: !5, flags: DIFlagArtificial, spFlags: DISPFlagDefinition, unit: !0)\n";
        }
        for (const auto& location : locations_) {
            out << "!" << location.block << " = !DILexicalBlockFile(scope: !" << location.scope
                << ", file: !" << location.file << ", discriminator: " << location.site << ")\n";
            out << "!" << location.id << " = !DILocation(line: " << location.line
                << ", column: " << location.column << ", scope: !" << location.block;
            if (location.inlined_at) out << ", inlinedAt: !" << location.inlined_at;
            out << ")\n";
        }
    }
    module.append(llvm_text::LlvmModule::Section::metadata, out.view());
}

FunctionCallSites::FunctionCallSites(CallSiteMetadata& metadata, const ModuleContext& context,
                                     const StatementAttribution* attribution, std::uint32_t owner,
                                     std::size_t scope, std::size_t file,
                                     std::uint32_t function_line, const ir::Function* function)
    : metadata_(metadata), context_(context), attribution_(attribution), owner_(owner),
      scope_(scope), file_(file), function_line_(function_line) {
    if (!function) return;
    // Each removed call is one real inline edge in the existing cold table.
    // Parent identity chooses the exact logical caller; no runtime stack work.
    std::size_t remaining=function->inline_scopes.size();
    while (remaining) {
        bool progress=false;
        for (const auto& frame:function->inline_scopes) {
            if (inline_locations_.contains(frame.identity)) continue;
            if (frame.parent && !inline_locations_.contains(frame.parent)) continue;
            const auto parent=frame.parent ? inline_locations_.at(frame.parent) : InlineLocation{owner_,scope_,file_,0};
            const auto callee=metadata_.symbol_function(abi::user_symbol(frame.callee));
            if (!callee) throw std::logic_error("inline call lacks its exact callee table identity");
            const auto& caller_file=frame.caller_location && !frame.caller_location->source_file.empty()
                ? frame.caller_location->source_file
                : frame.parent
                    ? std::find_if(function->inline_scopes.begin(),function->inline_scopes.end(),[&](const auto& f){return f.identity==frame.parent;})->source_file
                    : function->source_file;
            CallSite site;
            site.owner=parent.owner;site.callee=*callee;site.callee_kind=CallSiteCallee::function;
            if (const auto user=context_.user_source_index.find(caller_file);user!=context_.user_source_index.end()) {
                site.flags=call_site_flags::user;site.file=static_cast<std::uint32_t>(user->second);
                site.line=frame.position.line;site.column=frame.position.column;
            }
            const auto caller_debug_file=metadata_.mode()==CallSiteMetadata::Mode::debug
                ? metadata_.source_file_id(caller_file) : 0;
            const auto at=metadata_.locate(site,parent.scope,caller_debug_file,frame.position.line,frame.position.column,parent.at);
            const auto callee_scope=metadata_.subprogram(*callee);
            const auto callee_file=metadata_.mode()==CallSiteMetadata::Mode::debug
                ? metadata_.source_file_id(frame.source_file) : 0;
            inline_locations_.emplace(frame.identity,InlineLocation{*callee,callee_scope,callee_file,at});
            --remaining;progress=true;
        }
        if (!progress) throw std::logic_error("inline call metadata has cyclic or unavailable parents");
    }
}

std::size_t FunctionCallSites::call_location(std::string_view symbol) {
    CallSite site;
    const auto logical=inline_scope_ ? inline_locations_.at(inline_scope_) : InlineLocation{owner_,scope_,file_,0};
    site.owner = logical.owner;
    if (symbol.empty()) {
        site.callee_kind = CallSiteCallee::indirect;
    } else if (const auto function = metadata_.symbol_function(symbol)) {
        site.callee_kind = CallSiteCallee::function;
        site.callee = *function;
    } else if (const auto target = context_.call_targets.find(std::string(symbol));
               target != context_.call_targets.end() &&
               (target->second == CallTarget::foreign ||
                target->second == CallTarget::foreign_with_callback)) {
        site.callee_kind = CallSiteCallee::extern_function;
        if (target->second == CallTarget::foreign_with_callback)
            site.flags |= call_site_flags::extern_callback;
    } else {
        site.callee_kind = CallSiteCallee::runtime;
        if (symbol == runtime_abi::prelude::stack_enter.symbol()) site.flags |= call_site_flags::prologue;
    }
    if (synthesized_) site.flags |= call_site_flags::synthesized;
    std::uint32_t line = instruction_line_;
    std::uint32_t column = instruction_column_;
    if (line == 0 && attribution_) {
        line = attribution_->raw_statement_line();
        column = attribution_->raw_statement_column();
    }
    if (attribution_ && attribution_->user_code() && !synthesized_) {
        site.flags |= call_site_flags::user;
        site.file = static_cast<std::uint32_t>(attribution_->file_index());
        site.line = line;
        site.column = column;
    }
    if (line == 0) {
        line = function_line_;
        column = 0;
    }
    if (symbol == runtime_abi::tail::record.symbol()) site.flags |= call_site_flags::tail_record_helper;
    const auto* tail = tail_site_;
    // Producer-expanded returns cross explicit cleanup/end barriers, so
    // actual TailCallAnalysis cannot authorize a copied scope call. Keep
    // this invariant explicit before physical-frame tail bookkeeping.
    if (tail && inline_scope_) throw std::logic_error("inline scope call cannot carry a physical tail record");
    if (tail && (tail->callee.empty() ? symbol.empty() :
        symbol == abi::user_symbol(tail->callee) || symbol == abi::user_symbol(tail->callee) + ".depth")) {
        // Clear the pending site before emitting the record's own calls.
        tail_site_ = nullptr;
        site.flags |= call_site_flags::tail;
        if (tail->recorded) site.flags |= call_site_flags::recorded_tail;
        if (tail->self) site.flags |= call_site_flags::self_tail;
        const auto id = metadata_.next_site();
        const auto location = metadata_.locate(site, logical.scope, logical.file, line, column, logical.at);
        if (tail->recorded) tail_records_->record(id, tail->self);
        return location;
    }
    return metadata_.locate(site, logical.scope, logical.file, line, column, logical.at);
}

} // namespace quidra::llvm_backend
