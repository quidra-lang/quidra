#include "quidra/compiler_extension.hpp"
#include "quidra/compiler_extension_model.hpp"
#include "quidra/abi/dtype.hpp"
#include "quidra/compile_inputs.hpp"
#include "quidra/toml_subset.hpp"
#include "compiler_extension_expression.hpp"
#include "shape_relations.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace quidra {
namespace {
using Kind = CompilerExtensionTableKind;
using Fields = std::map<std::string, std::string>;
using Value = CompilerExtensionValue;
using Strings = std::vector<std::string>;
using StringMap = std::map<std::string, std::string>;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}
bool identifier(std::string_view text) {
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') || c == '_'; };
    if (text.empty() || !letter(text.front())) return false;
    return std::all_of(text.begin(), text.end(), [&](char c) {
        return letter(c) || (c >= '0' && c <= '9');
    });
}
[[noreturn]] void fail(std::string_view origin, std::string_view field, std::string_view message) {
    throw std::runtime_error("compiler extension " + std::string(field) + ": " +
                             std::string(message) + ": " + std::string(origin));
}
Strings list(std::string_view text, std::string_view origin, std::string_view field,
             bool allow_empty = false, char delimiter = ',') {
    Strings result;
    if (trim(text).empty() && allow_empty) return result;
    std::set<std::string> seen;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(delimiter, start);
        auto item = trim(text.substr(start, end == text.npos ? text.size() - start : end - start));
        if (item.empty()) fail(origin, field, "empty list item");
        if (!seen.insert(item).second) fail(origin, field, "duplicate list item '" + item + "'");
        result.push_back(std::move(item));
        if (end == text.npos) break;
        start = end + 1;
    }
    return result;
}
StringMap pairs(std::string_view text, std::string_view origin, std::string_view field,
                char separator = '=', bool allow_empty = false) {
    StringMap result;
    for (const auto& item : list(text, origin, field, allow_empty)) {
        const auto split = item.find(separator);
        if (split == item.npos || item.find(separator, split + 1) != item.npos)
            fail(origin, field, "expected name/value pair");
        auto key = trim(std::string_view(item).substr(0, split));
        auto value = trim(std::string_view(item).substr(split + 1));
        if (key.empty() || value.empty() || !result.emplace(key, value).second)
            fail(origin, field, "empty or duplicate map binding");
    }
    return result;
}
std::int64_t integer(std::string_view text, std::string_view origin, std::string_view field,
                     std::int64_t minimum = std::numeric_limits<std::int64_t>::min()) {
    std::int64_t result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || result < minimum)
        fail(origin, field, "invalid integer");
    return result;
}
std::vector<CompilerDeclarationSelector> selectors(std::string_view text,
                                                   std::string_view origin, std::string_view field) {
    std::vector<CompilerDeclarationSelector> result;
    Strings alternatives;
    std::size_t start = 0;
    int nesting = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i != text.size() && (text[i] == '(' || text[i] == '<')) ++nesting;
        if (i != text.size() && (text[i] == ')' || (text[i] == '>' && (i == 0 || text[i - 1] != '-')))) --nesting;
        if (nesting < 0) fail(origin, field, "unbalanced selector");
        if (i == text.size() || (text[i] == '|' && !nesting)) {
            auto alternative = trim(text.substr(start, i - start));
            if (alternative.empty()) fail(origin, field, "empty declaration selector");
            if (std::find(alternatives.begin(), alternatives.end(), alternative) != alternatives.end())
                fail(origin, field, "duplicate declaration selector");
            alternatives.push_back(std::move(alternative)); start = i + 1;
        }
    }
    if (nesting) fail(origin, field, "unbalanced selector");
    for (auto spelling : alternatives) {
        CompilerDeclarationSelector selector;
        selector.spelling = spelling;
        if (spelling.starts_with("$method.")) selector.kind = CompilerDeclarationSelector::Kind::Method;
        else if (spelling.starts_with("$construct")) selector.kind = CompilerDeclarationSelector::Kind::Constructor;
        int angles = 0, parentheses = 0;
        std::size_t signature = spelling.npos;
        for (std::size_t i = 0; i < spelling.size(); ++i) {
            const char c = spelling[i];
            if (c == '<') ++angles;
            if (c == '>' && (i == 0 || spelling[i - 1] != '-') && --angles < 0) fail(origin, field, "unbalanced selector");
            if (c == '(') {
                if (parentheses == 0 && angles == 0 && signature == spelling.npos) signature = i;
                ++parentheses;
            }
            if (c == ')' && --parentheses < 0) fail(origin, field, "unbalanced selector");
            if (signature != spelling.npos && parentheses == 0 && i + 1 != spelling.size())
                fail(origin, field, "selector has text after its signature");
        }
        if (angles || parentheses) fail(origin, field, "unbalanced selector");
        selector.name = trim(std::string_view(spelling).substr(0, signature));
        if (selector.name.empty()) fail(origin, field, "empty declaration name");
        if (signature != spelling.npos) selector.signature = spelling.substr(signature);
        result.push_back(std::move(selector));
    }
    return result;
}
std::vector<CompilerPodField> pod(std::string_view text, std::string_view origin, std::string_view field) {
    std::vector<CompilerPodField> result;
    std::set<std::string> names;
    std::uint64_t offset = 0;
    for (const auto& item : list(text, origin, field, true)) {
        const auto split = item.find(':');
        if (split == item.npos) fail(origin, field, "expected name:POD-type");
        CompilerPodField entry;
        entry.name = trim(std::string_view(item).substr(0, split));
        const auto type = trim(std::string_view(item).substr(split + 1));
        if (!identifier(entry.name) || !names.insert(entry.name).second)
            fail(origin, field, "invalid or duplicate POD field");
        if (type == "i32") entry.type = CompilerPodType::I32;
        else if (type == "i64") entry.type = CompilerPodType::I64;
        else if (type == "u32") entry.type = CompilerPodType::U32;
        else if (type == "f32") entry.type = CompilerPodType::F32;
        else if (type == "f64") entry.type = CompilerPodType::F64;
        else fail(origin, field, "unsupported POD type '" + type + "'");
        entry.size = entry.alignment = (type == "i64" || type == "f64") ? 8 : 4;
        if (offset > std::numeric_limits<std::uint64_t>::max() - entry.alignment + 1)
            fail(origin, field, "POD layout overflow");
        entry.offset = (offset + entry.alignment - 1) / entry.alignment * entry.alignment;
        if (entry.offset > std::numeric_limits<std::uint64_t>::max() - entry.size)
            fail(origin, field, "POD layout overflow");
        offset = entry.offset + entry.size;
        result.push_back(std::move(entry));
    }
    return result;
}

class FullParser {
public:
    FullParser(CompilerExtensionRegistration& registration, CompileInputs* inputs)
        : registration_(registration), inputs_(inputs), model_(std::make_shared<CompilerExtensionModel>()) {}
    std::shared_ptr<const CompilerExtensionModel> run();
private:
    CompilerExtensionRegistration& registration_;
    CompileInputs* inputs_;
    std::shared_ptr<CompilerExtensionModel> model_;
    std::string table_name_;
    const Fields* raw_{};
    CompilerExtensionTable* current_{};
    std::string key_name(std::string_view key) const { return table_name_ + "." + std::string(key); }
    [[noreturn]] void error(std::string_view key, std::string_view message) const {
        fail(registration_.descriptor_path, key_name(key), message);
    }
    const std::string* raw(std::string_view key) const {
        const auto found = raw_->find(std::string(key));
        return found == raw_->end() ? nullptr : &found->second;
    }
    std::string text(std::string_view key, bool required = false, std::string_view fallback = {}) {
        const auto* value = raw(key);
        if (!value && !required && fallback.empty()) return {};
        if ((!value && required) || (value && value->empty() && required)) error(key, "required non-empty field");
        std::string result = value ? *value : std::string(fallback);
        current_->fields[std::string(key)] = result;
        return result;
    }
    Strings strings(std::string_view key, bool required = false, bool empty = false,
                    std::string_view fallback = {}) {
        const auto* value = raw(key);
        if (!value && !required && fallback.empty()) return {};
        if (!value && required) error(key, "required field");
        auto result = list(value ? *value : fallback, registration_.descriptor_path, key_name(key), empty);
        current_->fields[std::string(key)] = result;
        return result;
    }
    StringMap mapping(std::string_view key, bool required = false, char separator = '=') {
        const auto* value = raw(key);
        if (!value && !required) return {};
        if (!value) error(key, "required field");
        auto result = pairs(*value, registration_.descriptor_path, key_name(key), separator);
        current_->fields[std::string(key)] = result;
        return result;
    }
    void boolean(std::string_view key, bool required = false, std::optional<bool> fallback = {}) {
        const auto* value = raw(key);
        if (!value && !required && !fallback) return;
        if (!value && required) error(key, "required field");
        if (value && *value != "true" && *value != "false") error(key, "expected true or false");
        current_->fields[std::string(key)] = value ? *value == "true" : *fallback;
    }
    void number(std::string_view key, std::optional<std::int64_t> fallback = {},
                std::int64_t minimum = std::numeric_limits<std::int64_t>::min()) {
        const auto* value = raw(key);
        if (!value && !fallback) return;
        current_->fields[std::string(key)] = value ? integer(*value, registration_.descriptor_path,
                                                                key_name(key), minimum) : *fallback;
    }
    std::string enumeration(std::string_view key, std::string_view options, bool required = false,
                            std::string_view fallback = {}) {
        const auto value = text(key, required, fallback);
        if (!raw(key) && !required && fallback.empty()) return {};
        const auto choices = list(options, registration_.descriptor_path, key_name(key));
        if (std::find(choices.begin(), choices.end(), value) == choices.end()) error(key, "invalid enum '" + value + "'");
        return value;
    }
    void list_enum(std::string_view key, std::string_view options, bool required = false,
                   std::string_view fallback = {}) {
        const auto values = strings(key, required, false, fallback);
        const auto choices = list(options, registration_.descriptor_path, key_name(key));
        for (const auto& value : values)
            if (std::find(choices.begin(), choices.end(), value) == choices.end()) error(key, "invalid enum '" + value + "'");
    }
    void expression(std::string_view key, CompilerExpression::Category category, bool required = false) {
        const auto* value = raw(key);
        if (!value && !required) return;
        if (!value) error(key, "required expression");
        current_->fields[std::string(key)] = parse_compiler_expression(*value, category,
            registration_.descriptor_path + " " + key_name(key));
    }
    void function(std::string_view key) {
        const auto value = text(key, true);
        current_->fields[std::string(key)] = selectors(value, registration_.descriptor_path, key_name(key));
    }
    void symbol(std::string_view key, bool required = false) {
        const auto value = text(key, required);
        if (raw(key) && !identifier(value)) error(key, "expected C symbol");
    }
    void layout(std::string_view key) {
        if (const auto* value = raw(key)) current_->fields[std::string(key)] = pod(*value, registration_.descriptor_path, key_name(key));
    }
    void keys(std::string_view allowed) const {
        const auto names = list(allowed, registration_.descriptor_path, table_name_);
        for (const auto& [key, value] : *raw_) {
            (void)value;
            if (std::find(names.begin(), names.end(), key) == names.end()) error(key, "unknown key");
        }
    }
    void reference(std::string_view key, std::string_view prefix, bool required = false) {
        const auto value = text(key, required);
        if (!raw(key) && !required) return;
        if (!model_->tables.contains(std::string(prefix) + "." + value)) error(key, "unknown " + std::string(prefix) + " '" + value + "'");
    }
    void explicit_surface(std::string_view key) const {
        const auto& operation = model_->tables.at("operation." + raw_->at(std::string(key)));
        const auto& values = std::get<std::vector<CompilerDeclarationSelector>>(operation.fields.at("function"));
        for (const auto& selector : values)
            if (selector.kind != CompilerDeclarationSelector::Kind::Function)
                error(key, "kernel/derivative requires an explicit function surface");
    }
    void constraints();
    void bindings(std::string_view key, bool required = false);
    void file();
    void table();
    void pattern();
};
void FullParser::constraints() {
    if (const auto* dtype = raw("dtype")) {
        if (!abi::canonical_tensor_dtype_name(*dtype)) error("dtype", "expected canonical tensor dtype");
        text("dtype");
    }
    number("rank", {}, 0);
    if (const auto* shape = raw("shape")) {
        std::vector<std::int64_t> result;
        for (const auto& extent : list(*shape, registration_.descriptor_path, key_name("shape"), true))
            result.push_back(integer(extent, registration_.descriptor_path, key_name("shape"), 0));
        current_->fields["shape"] = result;
        if (const auto* rank = raw("rank"); rank && integer(*rank, registration_.descriptor_path, key_name("rank"), 0) != static_cast<std::int64_t>(result.size()))
            error("shape", "rank and shape disagree");
    }
    enumeration("device", "cpu,gpu");
    enumeration("backend", "cpu,cuda,hip,metal,test");
    enumeration("layout", "contiguous,strided");
    boolean("tracked"); boolean("last_use"); boolean("owned");
    reference("policy", "execution_policy");
    if (const auto* relation = raw("same_shape")) {
        try { current_->fields["same_shape"] = shape_relation_parameters(*relation); }
        catch (const std::exception& e) { error("same_shape", e.what()); }
    }
}
void FullParser::bindings(std::string_view key, bool required) {
    for (const auto& [name, target] : mapping(key, required)) {
        if (!identifier(name)) error(key, "invalid binding symbol");
        const auto dot = target.find('.');
        const auto parameter = std::string_view(target).substr(0, dot);
        if (!identifier(parameter)) error(key, "invalid parameter binding");
        if (dot != target.npos) (void)integer(std::string_view(target).substr(dot + 1),
            registration_.descriptor_path, key_name(key), 0);
    }
}
void FullParser::file() {
    const auto configured = text("file", true);
    const std::filesystem::path relative(configured);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        error("file", "expected package-relative path");
    for (const auto& component : relative)
        if (component == "..") error("file", "parent path components are forbidden");
    const auto normalized = relative.lexically_normal();
    if (normalized.extension() != ".qk") error("file", "expected .qk source");
    const auto identity = normalized.generic_string();
    current_->fields["file"] = identity;
    if (registration_.files.contains(identity)) return;
    const auto path = std::filesystem::path(registration_.package_root) / normalized;
    if (probe_input_path(path, inputs_) != InputPathState::regular_file) error("file", "referenced source is not a regular file");
    auto content = read_input_file(path, InputFileKind::descriptor, inputs_);
    if (!content) error("file", "cannot read referenced source");
    registration_.files.emplace(identity, std::move(*content));
}
void FullParser::pattern() {
    keys("kind,nodes,edges,outputs,select,lower,fallback,numerics,priority");
    const auto kind = enumeration("kind", "vertical,horizontal", false, "vertical");
    const auto nodes = mapping("nodes", true);
    if (nodes.size() > 16) error("nodes", "at most 16 nodes are permitted");
    const auto classes = list("elementwise,duplicable,activation,reduction,linear-algebra,collective",
                              registration_.descriptor_path, key_name("nodes"));
    for (const auto& [name, operation] : nodes) {
        if (!identifier(name)) error("nodes", "invalid node name");
        if (operation.starts_with('@')) {
            auto trait = operation.substr(1);
            if (trait.ends_with('*')) trait.pop_back();
            if (std::find(classes.begin(), classes.end(), trait) == classes.end()) error("nodes", "unknown operation class");
        } else if (!model_->tables.contains("operation." + operation)) error("nodes", "unknown operation '" + operation + "'");
    }
    if (kind == "vertical") {
        const auto edges = strings("edges", true, true);
        std::map<std::string, Strings> outgoing;
        std::map<std::string, std::size_t> incoming;
        for (const auto& [name, operation] : nodes) { (void)operation; incoming[name] = 0; }
        for (const auto& edge : edges) {
            const auto arrow = edge.find("->");
            if (arrow == edge.npos || edge.find("->", arrow + 2) != edge.npos) error("edges", "expected node->node");
            const auto from = trim(std::string_view(edge).substr(0, arrow));
            const auto to = trim(std::string_view(edge).substr(arrow + 2));
            if (!nodes.contains(from) || !nodes.contains(to) || from == to) error("edges", "unknown or self edge");
            outgoing[from].push_back(to); ++incoming[to];
        }
        Strings ready;
        for (const auto& [name, count] : incoming) if (!count) ready.push_back(name);
        std::size_t processed = 0;
        for (std::size_t i = 0; i < ready.size(); ++i) {
            ++processed;
            for (const auto& next : outgoing[ready[i]]) if (--incoming[next] == 0) ready.push_back(next);
        }
        if (processed != nodes.size()) error("edges", "pattern contains a cycle");
        for (const auto& output : strings("outputs", true))
            if (!nodes.contains(output)) error("outputs", "unknown node");
    } else {
        if (raw("edges") && !raw("edges")->empty()) error("edges", "horizontal nodes must be independent");
        if (raw("outputs")) for (const auto& output : strings("outputs"))
            if (!nodes.contains(output)) error("outputs", "unknown node");
        const auto& first = nodes.begin()->second;
        for (const auto& [name, operation] : nodes) { (void)name;
            if (first != operation) error("nodes", "horizontal nodes must select one operation");
        }
    }
    const auto select = enumeration("select", "always,cost", true);
    const auto lower = text("lower", true);
    const auto separator = lower.find(':');
    if (separator == lower.npos) error("lower", "expected function:operation or kernel:kernel");
    const auto prefix = lower.substr(0, separator);
    const auto target = lower.substr(separator + 1);
    if ((prefix != "function" && prefix != "kernel") ||
        !model_->tables.contains((prefix == "function" ? "operation." : "kernel.") + target))
        error("lower", "unknown lowering target");
    reference("fallback", "operation");
    text("numerics", select == "cost"); number("priority", 0);
}
void FullParser::table() {
    switch (current_->kind) {
    case Kind::Extension:
        keys("version,phase"); number("version"); text("phase", true); break;
    case Kind::Documentation: case Kind::Passthrough:
        for (const auto& [key, value] : *raw_) current_->fields[key] = value;
        break;
    case Kind::Operation: {
        keys("function,semantic,traits,data,effects,reads,writes,inplace,capture,stream,untracked_only,validates,preconditions");
        function("function"); text("semantic");
        const auto traits = strings("traits", true);
        const auto known_traits = list("tensor,pure,stateful,differentiable,higher-order,inference,training,activation,fusion-target,specialization-target,backend-target,memory-reuse-target,training-target,elementwise,duplicable,reduction,linear-algebra,collective,finite-preserving", registration_.descriptor_path, key_name("traits"));
        for (const auto& trait : traits)
            if (std::find(known_traits.begin(), known_traits.end(), trait) == known_traits.end() &&
                !(trait.size() > 10 && trait.ends_with("-sensitive")) &&
                !(trait.size() > 7 && trait.starts_with("effect:"))) error("traits", "invalid trait '" + trait + "'");
        if (std::find(traits.begin(), traits.end(), "tensor") == traits.end() ||
            std::find(traits.begin(), traits.end(), "pure") == traits.end() ||
            std::find(traits.begin(), traits.end(), "stateful") != traits.end())
            error("traits", "operation requires tensor,pure and forbids stateful");
        strings("data"); strings("reads");
        list_enum("effects", "none,host-sync,rng,collective,alloc-outside-core,reads-policy,writes-policy,reads-state", false, "none");
        const auto& effects = std::get<Strings>(current_->fields.at("effects"));
        if (effects.size() > 1 && std::find(effects.begin(), effects.end(), "none") != effects.end()) error("effects", "none cannot be combined with effects");
        const auto writes = raw("writes") ? mapping("writes", false, ':') : StringMap{{"result", "full"}};
        for (const auto& [name, mode] : writes)
            if (!identifier(name) || (mode != "full" && mode != "partial")) error("writes", "expected parameter:full or parameter:partial");
        current_->fields["writes"] = writes;
        if (raw("inplace")) {
            StringMap aliases;
            for (const auto& pair : strings("inplace")) {
                const auto arrow = pair.find("->");
                if (arrow == pair.npos || pair.find("->", arrow + 2) != pair.npos) error("inplace", "expected parameter->result");
                const auto parameter = trim(std::string_view(pair).substr(0, arrow));
                const auto result = trim(std::string_view(pair).substr(arrow + 2));
                if (!identifier(parameter) || result != "result" || !aliases.emplace(parameter, result).second) error("inplace", "invalid or duplicate alias pair");
            }
            current_->fields["inplace"] = std::move(aliases);
        }
        enumeration("capture", "safe,unsafe", false, "unsafe");
        enumeration("stream", "compute,copy,communication", false, "compute");
        boolean("untracked_only"); list_enum("validates", "nonfinite");
        expression("preconditions", CompilerExpression::Category::Predicate);
        break;
    }
    case Kind::ExecutionPolicy:
        keys("function"); function("function"); break;
    case Kind::Fusion:
        keys("operations,replacement,semantic,priority,select");
        for (const auto& value : strings("operations", true))
            if (!model_->tables.contains("operation." + value)) error("operations", "unknown operation '" + value + "'");
        reference("replacement", "operation"); text("semantic"); number("priority", 0);
        enumeration("select", "always", false, "always"); break;
    case Kind::Pattern: pattern(); break;
    case Kind::Specialization: case Kind::Backend: case Kind::Memory:
        keys("operation,replacement,priority,dtype,rank,shape,device,backend,layout,tracked,last_use,owned,policy,same_shape");
        reference("operation", "operation", true); reference("replacement", "operation", true);
        number("priority", 0); constraints(); break;
    case Kind::Donation: {
        keys("operation,replacement,donate,dtype,backend,layout");
        reference("operation", "operation", true); reference("replacement", "operation", true);
        const auto donate = text("donate", true);
        const auto& fields = model_->tables.at("operation." + raw_->at("operation")).fields;
        const auto aliases = fields.find("inplace");
        if (aliases == fields.end() || !std::get<StringMap>(aliases->second).contains(donate)) error("donate", "parameter is absent from operation.inplace");
        constraints(); break;
    }
    case Kind::Kernel: {
        keys("operation,backend,role,admit,encode,file,entry,tensors,params,applies_when,bind,alias_safe,workspace,host_sync,numerics,epilogue,slots,outputs_extra,capture,cost,offsets");
        reference("operation", "operation", true); explicit_surface("operation");
        enumeration("backend", "cpu,metal,cuda,hip,test", true);
        const auto role = enumeration("role", "native,template,generated", true);
        if (role == "native") {
            symbol("admit", true); symbol("encode", true);
            for (const auto key : {"file", "entry", "slots"}) if (raw(key)) error(key, "field is unavailable for native role");
        } else {
            file(); text("entry", true);
            for (const auto key : {"admit", "encode"}) if (raw(key)) error(key, "field is unavailable for source role");
            if (role == "template") strings("slots", true);
            else if (raw("slots")) error("slots", "generated role has no slots");
        }
        strings("tensors", true); layout("params");
        expression("applies_when", CompilerExpression::Category::Predicate); bindings("bind");
        boolean("alias_safe", false, false);
        enumeration("workspace", "none,admit", false, "none");
        enumeration("host_sync", "none,status-buffer", false, "none");
        text("numerics", true); enumeration("epilogue", "none,f32-store,f64-store", false, "none");
        mapping("outputs_extra", false, ':'); enumeration("capture", "safe,unsafe");
        reference("cost", "cost"); enumeration("offsets", "any,zero", false, "any");
        break;
    }
    case Kind::Fragment:
        keys("file,entry,implements,dtype,numerics,backends"); file(); text("entry", true);
        reference("implements", "operation", true);
        if (!abi::canonical_tensor_dtype_name(text("dtype", true))) error("dtype", "expected canonical tensor dtype");
        text("numerics", true); list_enum("backends", "cpu,metal,cuda,hip,test", true); break;
    case Kind::Intrinsic:
        keys("metal,ptx,llvm,hip,numerics");
        if (!raw("metal") && !raw("ptx") && !raw("llvm") && !raw("hip")) error("metal", "intrinsic requires at least one target");
        for (const auto key : {"metal", "ptx", "llvm", "hip"}) if (raw(key)) text(key, true);
        text("numerics", true); break;
    case Kind::Cost:
        keys("bind,flops,bytes_read,bytes_written,launches,efficiency,shared,threads"); bindings("bind", true);
        for (const auto key : {"flops", "bytes_read", "bytes_written", "launches"}) expression(key, CompilerExpression::Category::Formula, true);
        expression("shared", CompilerExpression::Category::Formula); expression("threads", CompilerExpression::Category::Formula);
        for (const auto& [backend, ratio] : mapping("efficiency")) {
            if (backend != "cpu" && backend != "metal" && backend != "cuda" && backend != "hip" && backend != "test") error("efficiency", "unknown backend");
            (void)integer(ratio, registration_.descriptor_path, key_name("efficiency"), 0);
        }
        break;
    case Kind::Derivative: {
        keys("operation,differentiable,saves,metadata,vjp_native,vjp_function,vjp_higher_order,primal_untracked_equivalent,recompute,residual,residual_vjp_native,mask,deterministic_under,full_writes");
        reference("operation", "operation", true); explicit_surface("operation");
        const auto differentiable = strings("differentiable", true);
        strings("saves", true, true); layout("metadata");
        if (!raw("vjp_native") && !raw("vjp_function")) error("vjp_native", "requires vjp_native or vjp_function");
        symbol("vjp_native"); reference("vjp_function", "operation"); reference("vjp_higher_order", "operation");
        boolean("primal_untracked_equivalent", true); enumeration("recompute", "allowed,never,elementwise", true);
        if (raw("residual")) { text("residual", true); symbol("residual_vjp_native", true); }
        else if (raw("residual_vjp_native")) error("residual_vjp_native", "requires residual");
        boolean("mask"); list_enum("deterministic_under", "deterministic,fast", false, "deterministic,fast");
        for (const auto& name : strings("full_writes"))
            if (std::find(differentiable.begin(), differentiable.end(), name) == differentiable.end()) error("full_writes", "parameter is not differentiable");
        break;
    }
    case Kind::Tuning:
        keys("kernel,key,space,numerics,policy,repeats"); reference("kernel", "kernel", true);
        strings("key", true);
        for (const auto& [name, alternatives] : mapping("space", true)) {
            if (!identifier(name)) error("space", "invalid tuning parameter");
            (void)list(alternatives, registration_.descriptor_path, key_name("space"), false, '|');
        }
        enumeration("numerics", "invariant,tolerance", true); enumeration("policy", "any,fast-only", true);
        number("repeats", 5, 1); break;
    case Kind::Collective: {
        keys("operation,combine,combine_numerics_deterministic,combine_numerics_fast");
        reference("operation", "operation", true); reference("combine", "operation");
        const auto& traits = std::get<Strings>(model_->tables.at("operation." + raw_->at("operation")).fields.at("traits"));
        if (std::find(traits.begin(), traits.end(), "collective") == traits.end()) error("operation", "requires collective trait");
        enumeration("combine_numerics_deterministic", "bitwise,tolerance,forbidden", false, "forbidden");
        enumeration("combine_numerics_fast", "bitwise,tolerance,forbidden", false, "forbidden"); break;
    }
    case Kind::Query:
        keys("function,depends"); function("function"); list_enum("depends", "backend,device,dtype,contiguous,rank", true); break;
    }
}
std::shared_ptr<const CompilerExtensionModel> FullParser::run() {
    const std::map<std::string, Kind> kinds = {
        {"operation", Kind::Operation}, {"execution_policy", Kind::ExecutionPolicy},
        {"fusion", Kind::Fusion}, {"pattern", Kind::Pattern}, {"specialization", Kind::Specialization},
        {"backend", Kind::Backend}, {"memory", Kind::Memory}, {"donation", Kind::Donation},
        {"kernel", Kind::Kernel}, {"fragment", Kind::Fragment}, {"intrinsic", Kind::Intrinsic},
        {"cost", Kind::Cost}, {"derivative", Kind::Derivative}, {"tuning", Kind::Tuning},
        {"collective", Kind::Collective}, {"query", Kind::Query}};
    for (const auto& [name, fields] : registration_.tables) {
        (void)fields;
        CompilerExtensionTable entry;
        if (name == "extension") entry.kind = Kind::Extension;
        else if (name == "policy" || name == "analysis") entry.kind = Kind::Documentation;
        else {
            const auto dot = name.find('.');
            const auto prefix = name.substr(0, dot);
            if (const auto found = kinds.find(prefix); found != kinds.end()) {
                if (dot == name.npos || dot + 1 == name.size()) fail(registration_.descriptor_path, name, "requires non-empty id");
                entry.kind = found->second; entry.id = name.substr(dot + 1);
            } else if (prefix.starts_with("x-")) { entry.kind = Kind::Passthrough; entry.id = dot == name.npos ? "" : name.substr(dot + 1); }
            else fail(registration_.descriptor_path, name, "unknown table prefix");
        }
        model_->tables.emplace(name, std::move(entry));
    }
    // References can point forward. Contracts needed by other tables are
    // normalized first, without changing raw descriptor table order/bytes.
    for (const bool operations : {true, false}) for (auto& [name, entry] : model_->tables) {
        if ((entry.kind == Kind::Operation) != operations) continue;
        table_name_ = name; raw_ = &registration_.tables.at(name); current_ = &entry; table();
    }
    for (const auto& [name, entry] : model_->tables) {
        if (entry.kind != Kind::Fragment) continue;
        const auto& operation = std::get<std::string>(entry.fields.at("implements"));
        const auto& backends = std::get<Strings>(entry.fields.at("backends"));
        for (const auto& [kernel_name, kernel] : model_->tables) {
            (void)kernel_name;
            if (kernel.kind != Kind::Kernel || std::get<std::string>(kernel.fields.at("operation")) != operation) continue;
            const auto& backend = std::get<std::string>(kernel.fields.at("backend"));
            if (std::find(backends.begin(), backends.end(), backend) != backends.end() &&
                entry.fields.at("numerics") != kernel.fields.at("numerics"))
                fail(registration_.descriptor_path, name + ".numerics", "differs from declared unfused kernel");
        }
    }
    return model_;
}

void validate_legacy(const TomlDocument& document, const std::filesystem::path& descriptor_path) {
for (const auto& [table_name, fields] : document.tables) {
    if (!table_name.starts_with("specialization.") &&
        !table_name.starts_with("backend.") &&
        !table_name.starts_with("memory.")) continue;
    const auto dtype = fields.find("dtype");
    if (dtype != fields.end() &&
        !abi::canonical_tensor_dtype_name(dtype->second)) {
        throw std::runtime_error(
            "compiler extension rule '" + table_name +
            "' requires a canonical tensor dtype, not '" +
            dtype->second + "': " + descriptor_path.string());
    }
}
std::unordered_set<std::string> operation_ids;
for (const auto& [table_name, fields] :
     document.tables) {
    if (!table_name.starts_with("operation.")) continue;
    const auto operation_id = table_name.substr(
        std::string("operation.").size());
    if (operation_id.empty()) {
        throw std::runtime_error(
            "compiler extension operation table requires "
            "a non-empty id: " +
            descriptor_path.string());
    }
    const auto function = fields.find("function");
    if (function == fields.end() ||
        function->second.empty()) {
        throw std::runtime_error(
            "compiler extension operation '" +
            operation_id +
            "' requires a non-empty function: " +
            descriptor_path.string());
    }
    operation_ids.insert(operation_id);
}

std::unordered_set<std::string> execution_policy_ids;
for (const auto& [table_name, fields] :
     document.tables) {
    if (!table_name.starts_with(
            "execution_policy.")) {
        continue;
    }
    const auto policy_id = table_name.substr(
        std::string("execution_policy.").size());
    const auto function = fields.find("function");
    if (policy_id.empty() ||
        function == fields.end() ||
        function->second.empty()) {
        throw std::runtime_error(
            "compiler extension execution policy table "
            "requires a non-empty id and function: " +
            descriptor_path.string());
    }
    execution_policy_ids.insert(policy_id);
}

for (const auto& [table_name, fields] :
     document.tables) {
    if (!table_name.starts_with("fusion.")) continue;
    const auto fusion_id = table_name.substr(
        std::string("fusion.").size());
    const auto configured = fields.find("operations");
    if (fusion_id.empty() ||
        configured == fields.end() ||
        configured->second.empty()) {
        throw std::runtime_error(
            "compiler extension fusion table requires "
            "a non-empty id and operations list: " +
            descriptor_path.string());
    }

    std::size_t start = 0;
    while (start <= configured->second.size()) {
        const auto comma =
            configured->second.find(',', start);
        const auto end =
            comma == std::string::npos
                ? configured->second.size()
                : comma;
        auto operation = configured->second.substr(
            start, end - start);
        const auto first =
            operation.find_first_not_of(" \t\r");
        const auto last =
            operation.find_last_not_of(" \t\r");
        if (first == std::string::npos) {
            throw std::runtime_error(
                "compiler extension fusion '" +
                fusion_id +
                "' contains an empty operation id: " +
                descriptor_path.string());
        }
        operation =
            operation.substr(first, last - first + 1);
        if (!operation_ids.contains(operation)) {
            throw std::runtime_error(
                "compiler extension fusion '" +
                fusion_id +
                "' references unknown operation '" +
                operation + "': " +
                descriptor_path.string());
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    if (const auto replacement =
            fields.find("replacement");
        replacement != fields.end()) {
        if (replacement->second.empty() ||
            !operation_ids.contains(replacement->second)) {
            throw std::runtime_error(
                "compiler extension fusion '" +
                fusion_id +
                "' references unknown replacement operation '" +
                replacement->second + "': " +
                descriptor_path.string());
        }
    }
}
for (const auto& [table_name, fields] :
     document.tables) {
    std::string rule_kind;
    std::string rule_prefix;
    if (table_name.starts_with("specialization.")) {
        rule_kind = "specialization";
        rule_prefix = "specialization.";
    } else if (table_name.starts_with("backend.")) {
        rule_kind = "backend";
        rule_prefix = "backend.";
    } else if (table_name.starts_with("memory.")) {
        rule_kind = "memory";
        rule_prefix = "memory.";
    } else {
        continue;
    }

    const auto rule_id =
        table_name.substr(rule_prefix.size());
    const auto operation = fields.find("operation");
    const auto replacement = fields.find("replacement");
    if (rule_id.empty() ||
        operation == fields.end() ||
        operation->second.empty() ||
        replacement == fields.end() ||
        replacement->second.empty()) {
        throw std::runtime_error(
            "compiler extension " + rule_kind +
            " table requires a non-empty id, operation, "
            "and replacement: " +
            descriptor_path.string());
    }
    if (!operation_ids.contains(operation->second)) {
        throw std::runtime_error(
            "compiler extension " + rule_kind + " '" +
            rule_id +
            "' references unknown operation '" +
            operation->second + "': " +
            descriptor_path.string());
    }
    if (!operation_ids.contains(replacement->second)) {
        throw std::runtime_error(
            "compiler extension " + rule_kind + " '" +
            rule_id +
            "' references unknown replacement operation '" +
            replacement->second + "': " +
            descriptor_path.string());
    }
    if (const auto relation = fields.find("same_shape"); relation != fields.end())
        (void)shape_relation_parameters(relation->second);
    if (const auto policy = fields.find("policy");
        policy != fields.end()) {
        if (policy->second.empty() ||
            !execution_policy_ids.contains(
                policy->second)) {
            throw std::runtime_error(
                "compiler extension " + rule_kind + " '" +
                rule_id +
                "' references unknown execution policy '" +
                policy->second + "': " +
                descriptor_path.string());
        }
    }
}
}
} // namespace

CompilerExtensionRegistration parse_compiler_extension_descriptor(
    std::string package, std::string name, const std::filesystem::path& package_root,
    const std::filesystem::path& descriptor_path, std::string descriptor, CompileInputs* inputs) {
    const auto document = parse_toml_subset(descriptor, descriptor_path.string());
    const auto* version = document.find("extension", "version");
    const auto* phase = document.find("extension", "phase");
    if (!version || *version != "2") throw std::runtime_error(
        "compiler extension descriptor requires [extension] version = 2: " + descriptor_path.string());
    if (!phase || (*phase != "tensor-region" && *phase != "tensor-program")) throw std::runtime_error(
        "compiler extension descriptor has unsupported phase; expected 'tensor-region': " + descriptor_path.string());
    CompilerExtensionRegistration result{std::move(package), std::move(name), package_root.string(),
        descriptor_path.string(), std::move(descriptor), *phase, document.tables};
    if (*phase == "tensor-region") validate_legacy(document, descriptor_path);
    else { result.schema_revision = 2; result.model = FullParser(result, inputs).run(); }
    return result;
}
} // namespace quidra
