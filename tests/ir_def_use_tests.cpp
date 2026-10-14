#include <cstdio>
#include "ir/analysis/def_use.hpp"
#include "ir/verify.hpp"
#include "quidra/ir/module.hpp"
#include "quidra/ir/initialization_mask.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <vector>
namespace {
using namespace quidra;
using namespace quidra::ir;
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
void sparse_occurrences() {
    const auto maximum=std::numeric_limits<ValueId>::max();
    Function function;
    function.next_value=std::uint64_t{maximum}+1;
    function.blocks={{"entry",{ConstantBool{1,true},ConstantBool{maximum,false},
        SourceLocation{{17,19},"generic.qui","","node","statement"},
        Binary{2,"and",1,1,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)},TestAssert{maximum},ReturnVoid{}}}};
    const auto chain=analysis::def_use(function);
    require(chain.values.size()==3&&chain.max_value==maximum,"sparse IDs must allocate only actual value records");
    require(chain.find(1)&&chain.find(1)->uses.size()==2,"repeated operand occurrences must not be deduplicated");
    require(chain.find(1)->uses[0].ordinal==0&&chain.find(1)->uses[1].ordinal==1,"authored use ordinal retained");
    require(chain.find(1)->uses[0].at==analysis::InstrRef{0,3}&&chain.find(maximum)->uses.front().at==analysis::InstrRef{0,4},
        "source-location instructions count in references and high IDs remain usable");
    require(verify(function).ssa_valid,"valid sparse function must verify");
}
struct Roles final : OperandVisitor {
    std::vector<ValueId> values;
    std::vector<OperandUse> operands;
    void use(ValueId value, OperandUse operand) override { values.push_back(value); operands.push_back(operand); }
};
Roles roles(const Instruction& instruction, const Module* module = nullptr) {
    Roles result;
    visit_operands(instruction, result, OperandContext{module});
    return result;
}
void expect(const Roles& actual, std::initializer_list<ValueId> values,
            std::initializer_list<OperandUse> operands, const char* message) {
    require(actual.values == std::vector<ValueId>(values) &&
        actual.operands == std::vector<OperandUse>(operands), message);
}
const Type boolean = Type::simple(TypeKind::Bool);
const Type text = Type::simple(TypeKind::String);
const Type exact = Type::simple(TypeKind::Real);
constexpr OperandUse read{OperandRole::Read,true}, borrow{OperandRole::Borrow,true},
    consume{OperandRole::Consume,true}, address{OperandRole::Address,true},
    condition{OperandRole::Condition,true}, target_role{OperandRole::AutogradTarget,true},
    unknown{OperandRole::Read,false};

void local_and_domain_roles() {
    expect(roles(StoreLocal{"slot",1,text}), {1},{consume}, "owned store consumes new owner");
    expect(roles(StoreLocal{"slot",1,text,true}), {1},{borrow}, "borrowed store lends new owner");
    expect(roles(StoreLocal{"slot",1,boolean}), {1},{read}, "scalar storage is an ordinary read");
    expect(roles(StoreAddress{1,2,exact}), {1,2},{address,consume}, "address store uses shared exact ownership predicate");
    expect(roles(StoreReference{"ref",2,text}), {2},{consume}, "reference target consumes transferred value");
    expect(roles(Release{1,text}), {1},{consume}, "release consumes owner");
    expect(roles(Retain{2,1,text}), {1},{borrow}, "retain borrows source owner");
    expect(roles(Return{1,text}), {1},{consume}, "managed return consumes value");
    expect(roles(Branch{1,"yes","no"}), {1},{condition}, "branch is a condition use");
    expect(roles(TestAssert{1}), {1},{condition}, "assert is a condition use");
    expect(roles(BindReference{"ref",1}), {1},{address}, "reference binding uses address");
    expect(roles(TensorBackward{1,{{2,false},{3,true}},4,5}), {1,2,3,4,5},
        {borrow,borrow,target_role,target_role,condition}, "autograd descriptors retain typed roles");
    expect(roles(TupleMake{3,{1,2},Type::tuple({text,boolean})}), {1,2},{consume,read},
        "tuple child transfers follow child types");
    expect(roles(TupleMake{3,{1,2},Type::tuple({text})}), {1,2},{unknown,unknown},
        "tuple arity mismatch preserves all unresolved uses");
    expect(roles(ArrayMake{3,{1,2},Type::array(text)}), {1,2},{consume,consume},
        "array construction consumes owned elements");
    expect(roles(ArraySet{1,2,3,text,{0,0},false,false,4,5,6}), {1,2,3,4,5,6},
        {borrow,read,consume,condition,condition,address}, "array setter roles preserve guard and receipt uses");
    expect(roles(ArraySliceSet{1,{2,3,4},5,Type::array(text)}), {1,2,3,4,5},
        {borrow,read,read,read,borrow}, "slice setter copies borrowed source");
    expect(roles(StringBuildAppendMove{6,7,1,{{2,text},{3,boolean}},4}), {1,2,3,4},
        {consume,borrow,read,borrow}, "unique append consumes receiver but borrows source parts");
    expect(roles(StringAppendMove{3,1,{2}}), {1,2},{unknown,borrow},
        "conditional append receiver ownership is explicitly unresolved");
    expect(roles(TupleGet{2,1,0,text,true}), {1},{unknown},
        "partial tuple projection is not whole-aggregate consumption");
    expect(roles(StoreLocal{"slot",1,Type::simple(TypeKind::Auto)}), {1},{unknown},
        "missing typed ownership contract cannot become scalar read proof");
    Module module;
    module.classes.push_back({"Pair",{"a","b"},{text,boolean},false,{}});
    const ClassMake object{3,Type::class_type("Pair"),{1,2}};
    expect(roles(object), {1,2},{unknown,unknown}, "nominal fields require typed layout");
    expect(roles(object,&module), {1,2},{consume,read}, "class fields use typed layout");
    module.classes.front().field_names.emplace_back(initialization_mask_field);
    module.classes.front().fields.push_back(Type::simple(TypeKind::Nat64));
    expect(roles(object,&module), {1,2},{consume,read}, "hidden initialization mask does not invalidate supplied class fields");
    module.classes.push_back(module.classes.front());
    expect(roles(object,&module), {1,2},{unknown,unknown}, "ambiguous class layouts are unavailable");
}

void call_roles() {
    Module module;
    Function callee;
    callee.name="callee";
    callee.parameters={{"own",text},{"lend",text,false,true},{"scalar",boolean},{"ref",text,true}};
    module.functions.push_back(callee);
    const Call call{0,"callee",{{1,{}},{2,{}},{3,{}},{0,4}},Type::simple(TypeKind::Void)};
    expect(roles(call), {1,2,3,4},{unknown,unknown,unknown,address}, "missing direct target context keeps actual uses");
    expect(roles(call,&module), {1,2,3,4},{consume,borrow,read,address}, "typed call contract distinguishes owner, loan, scalar, address");
    auto malformed=call;
    malformed.result=boolean;
    expect(roles(malformed,&module), {1,2,3,4},{unknown,unknown,unknown,address}, "call result type mismatch is unavailable");
    malformed=call; malformed.args.pop_back();
    expect(roles(malformed,&module), {1,2,3},{unknown,unknown,unknown}, "call arity mismatch retains every use");
    module.functions.push_back(callee);
    expect(roles(call,&module), {1,2,3,4},{unknown,unknown,unknown,address}, "ambiguous callee cannot prove consumption");
    module.functions.pop_back();
    module.functions.front().external_symbol="foreign";
    expect(roles(call,&module), {1,2,3,4},{borrow,borrow,read,address}, "native string ABI lends buffers");
    module.functions.front().parameters.front().type=Type::array(text);
    expect(roles(call,&module), {1,2,3,4},{unknown,unknown,unknown,address}, "unsupported native managed contract remains unavailable");
    module.functions.front().parameters.front().type=Type::simple(TypeKind::Auto);
    expect(roles(call,&module), {1,2,3,4},{unknown,unknown,unknown,address}, "missing target type remains unavailable");
    expect(roles(IndirectCall{0,1,{2,3,4},{text,boolean,Type::function(text,{text})},Type::simple(TypeKind::Void)}),
        {1,2,3,4},{read,consume,read,read}, "indirect typed signature consumes managed values but not function signature children");
    expect(roles(IndirectCall{0,1,{2,3},{text},Type::simple(TypeKind::Void)}),
        {1,2,3},{read,unknown,unknown}, "indirect arity mismatch keeps all arguments");
    module.functions.front()=callee;
    ParallelBlock parallel{};
    parallel.out=5;
    parallel.nodes={{"callee",call.args,{}}};
    parallel.iterations=ParallelIteration{6,7,{{0,8,false,false}}};
    expect(roles(parallel,&module), {1,2,3,4,6,7,8},{consume,borrow,read,address,read,condition,borrow},
        "parallel captures resolve one typed target per node and retain iteration receipts");
    // SSA validity and ownership resolution are independent facts.
    Function caller;
    caller.blocks={{"entry",{ConstantBool{1,true},Call{0,"absent",{{1,{}}},Type::simple(TypeKind::Void)},ReturnVoid{}}}};
    const auto report=verify(caller);
    require(report.ssa_valid && report.dominance_complete && !report.roles_complete && report.unresolved_roles.size()==1,
        "valid SSA does not pretend unresolved ownership is complete");
}

void nested_context() {
    Module outer, inner;
    Function owning; owning.name="target"; owning.parameters={{"value",text}};
    outer.functions.push_back(owning);
    owning.parameters.front().borrowed=true;
    inner.functions.push_back(owning);
    struct Nested final : OperandVisitor {
        const Module& outer;
        const Module& inner;
        bool nested{}, throw_now{};
        std::vector<OperandUse> operands;
        Nested(const Module& a,const Module& b):outer(a),inner(b){}
        void use(ValueId,OperandUse operand) override {
            operands.push_back(operand);
            if (nested) {
                require(context()->module==&inner,"nested traversal uses inner context");
                if (throw_now) throw std::runtime_error("nested rejection");
                return;
            }
            require(context()->module==&outer,"outer traversal retains outer context");
            nested=true;
            try { visit_operands(Call{0,"target",{{2,{}}},Type::simple(TypeKind::Void)},*this,OperandContext{&inner}); }
            catch(const std::runtime_error& error) {require(std::string(error.what())=="nested rejection","original nested exception retained");}
            nested=false;
            require(context()->module==&outer,"nested traversal restores outer context even on exception");
        }
    } visitor(outer,inner);
    const Call call{0,"target",{{1,{}}},Type::simple(TypeKind::Void)};
    visit_operands(call,visitor,OperandContext{&outer});
    require(visitor.operands==std::vector<OperandUse>{consume,borrow}&&visitor.context()==nullptr,
        "nested contexts restore initial null after normal traversal");
    visitor.throw_now=true;visitor.operands.clear();
    visit_operands(call,visitor,OperandContext{&outer});
    require(visitor.operands==std::vector<OperandUse>{consume,borrow}&&visitor.context()==nullptr,
        "nested exceptions restore context after outer traversal");
    struct Throw final:OperandVisitor {void use(ValueId,OperandUse) override {throw std::runtime_error("stop");}} throwing;
    try { visit_operands(call,throwing,OperandContext{&outer});require(false,"exception expected"); }
    catch(const std::runtime_error&) {}
    require(throwing.context()==nullptr,"escaping exceptions restore visitor context");
}

bool has(const VerifyReport& report,SsaIssue issue) {
    return std::any_of(report.diagnostics.begin(),report.diagnostics.end(),[&](const auto& diagnostic){return diagnostic.issue==issue;});
}
void ssa_invalidity() {
    Function f;
    f.blocks={{"entry",{TestAssert{1},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::MissingDefinition),"use-only value rejected");
    f.blocks={{"entry",{ConstantBool{0,true},TestAssert{0},ReturnVoid{}}}};
    require(verify(f).diagnostics.size()==2&&has(verify(f),SsaIssue::ZeroValue),"genuine zero definition and use both rejected");
    f.blocks={{"entry",{TestAssert{1},ConstantBool{1,true},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::DefinitionNotEarlier),"forward same-block use rejected");
    f.blocks={{"entry",{Binary{1,"and",1,1,boolean,boolean},ReturnVoid{}}}};
    const auto self=verify(f);
    require(self.diagnostics.size()==2&&self.diagnostics[0].ordinal==0&&self.diagnostics[1].ordinal==1&&
        has(self,SsaIssue::DefinitionNotEarlier),"same-instruction definition never validates repeated self uses");
    f.blocks={{"entry",{ConstantBool{1,true},ConstantBool{1,false},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::MultipleDefinitions)&&verify(f).diagnostics.size()==2,"duplicate definitions retained without a privileged winner");
    f.blocks={{"entry",{ConstantBool{1,true},ReturnVoid{}}},{"dead",{ConstantBool{1,false},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::MultipleDefinitions),"global uniqueness includes unreachable definitions");
    f.blocks={{"entry",{ConstantBool{1,true},Branch{1,"left","right"}}},
        {"left",{ConstantBool{2,true},Jump{"merge"}}},{"right",{Jump{"merge"}}},
        {"merge",{TestAssert{2},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::DefinitionDoesNotDominate),"one-arm diamond definition does not dominate merge use");
    f.blocks={{"entry",{Jump{"header"}}},{"header",{TestAssert{2},Jump{"body"}}},
        {"body",{ConstantBool{2,true},Jump{"header"}}}};
    require(has(verify(f),SsaIssue::DefinitionDoesNotDominate),"backedge definition cannot validate first header iteration");
    f.blocks={{"entry",{TestAssert{2},ReturnVoid{}}},{"dead",{ConstantBool{2,true},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::DefinitionDoesNotDominate),"unreachable definition cannot dominate reachable use");
}

void scope_and_sentinels() {
    Function f;
    f.blocks={{"entry",{ConstantBool{1,true},Jump{"header"}}},
        {"header",{TestAssert{1},Jump{"body"}}},{"body",{Jump{"header"}}}};
    require(verify(f).ssa_valid&&verify(f).dominance_complete,"valid loop invariant verifies");
    f.blocks={{"entry",{Jump{"definition"}}},{"use",{TestAssert{1},ReturnVoid{}}},
        {"definition",{ConstantBool{1,true},Jump{"use"}}}};
    require(verify(f).ssa_valid,"authored block order is not dominance order");
    f.blocks={{"entry",{ReturnVoid{}}},{"dead-definition",{ConstantBool{1,true},ReturnVoid{}}},
        {"dead-use",{TestAssert{1},ReturnVoid{}}}};
    const auto incomplete=verify(f);
    require(incomplete.ssa_valid&&!incomplete.dominance_complete&&incomplete.roles_complete&&
        incomplete.unverified_dominance.size()==1&&incomplete.unverified_dominance.front().use.at==analysis::InstrRef{2,0},
        "unreachable cross-block dominance remains explicitly unverified");
    f.blocks={{"entry",{ReturnVoid{}}},{"dead",{TestAssert{1},ConstantBool{1,true},ReturnVoid{}}}};
    require(!verify(f).ssa_valid&&has(verify(f),SsaIssue::DefinitionNotEarlier),"unreachable same-block strict order still checked");
    f.blocks={{"entry",{BindReference{"ref",0},VariantMake{1,0,0,{},Type::simple(TypeKind::Void)},
        ReplDisplay{0,Type::simple(TypeKind::None),{}},ReturnVoid{}}}};
    require(analysis::def_use(f).find(0)==nullptr&&verify(f).ssa_valid,"documented absent-value sentinels never become malformed SSA");
    f.blocks={{"entry",{ParallelBlock{},ReturnVoid{}}}};
    require(has(verify(f),SsaIssue::ZeroValue),"nullable parallel result must still have a nonzero SSA ID");
    f.blocks={{"entry",{ConstantString{4,"text"},ParseNumberDirect{1,2,3,4,boolean},
        TestAssert{2},ReturnVoid{}}}};
    const auto multi=analysis::def_use(f);
    require(multi.values.size()==4&&multi.find(1)->definitions.front()==analysis::InstrRef{0,1}&&
        multi.find(2)->definitions.front()==analysis::InstrRef{0,1}&&multi.find(3)->definitions.front()==analysis::InstrRef{0,1}&&verify(f).ssa_valid,
        "multiple output instructions retain all SSA definitions");
    const auto before=analysis::def_use(f);
    const auto report1=verify(f),report2=verify(f);
    const auto after=analysis::def_use(f);
    require(before.values.size()==after.values.size()&&before.find(4)->uses==after.find(4)->uses&&
        f.blocks.front().label=="entry"&&std::get<ConstantString>(f.blocks.front().instructions.front()).value=="text"&&
        report1.diagnostics==report2.diagnostics,"analysis is observational and deterministic");
    f.blocks={{"entry",{Jump{"absent"}}}};
    try { (void)verify(f);require(false,"malformed CFG expected"); }
    catch(const std::invalid_argument&) {}
    f.blocks.clear();
    require(verify(f).ssa_valid&&analysis::def_use(f).values.empty(),"empty declaration is valid empty analysis");
}

// Frozen legacy callback sequences for populated representatives of every
// Instruction alternative. Nested operands, authored repetition and storage/
// successor names are populated independently of the new role dispatcher.
template<class T> void fill(T&, ValueId) {}
void fill(ValueId& field, ValueId id) { field=id; }
void fill(std::optional<ValueId>& field, ValueId id) { field=id; }
void fill(std::vector<ValueId>& field, ValueId id) { field={id,id+1,id}; }
void fill(std::vector<std::optional<ValueId>>& field, ValueId id) { field={id,{},id+1}; }
void fill(std::string& field, ValueId id) { field="name"+std::to_string(id); }
void fill(SliceBounds& field, ValueId id) {field={id,id+1,id+2};}
void fill(std::vector<CallArgument>& field, ValueId id) {field={{id,{}},{0,id+1},{id+2,id+3}};}
void fill(std::vector<StringBuildPart>& field, ValueId id) {field={{id,Type::simple(TypeKind::String)},{id+1,Type::simple(TypeKind::Bool)}};}
void fill(std::vector<TensorIndexPart>& field, ValueId id) {field={{0,id,id+1,id+2,id+3},{1,{},id+4,{},id+5}};}
void fill(CheckedReal& field, ValueId id) {field={id,Type::simple(TypeKind::Real32),{701,702},FiniteCause::division,id+1,id+2};}
void fill(std::vector<CheckedReal>& field, ValueId id) { CheckedReal a{};fill(a,id);field={a,a}; }
void fill(std::vector<TensorBackwardTarget>& field, ValueId id) {field={{id,false},{id+1,true}};}
void fill(std::vector<ParallelNode>& field, ValueId id) {field={{"worker",{{id,{}},{0,id+1},{id+2,id+3}},{0},{901,902}}};}
void fill(std::optional<ParallelIteration>& field, ValueId id) {field=ParallelIteration{id,id+1,{{0,id+2,false,false},{1,id+3,true,false}}};}
template<class T> T populated() {
    T n = [] { if constexpr(std::is_same_v<T,Binary>) return Binary{301,"+",11,12,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64)};else return T{};}();
    if constexpr(requires { n.out; }) fill(n.out, 11);
    if constexpr(requires { n.name; }) fill(n.name, 21);
    if constexpr(requires { n.object; }) fill(n.object, 31);
    if constexpr(requires { n.array; }) fill(n.array, 41);
    if constexpr(requires { n.index; }) fill(n.index, 51);
    if constexpr(requires { n.address; }) fill(n.address, 61);
    if constexpr(requires { n.value; }) fill(n.value, 71);
    if constexpr(requires { n.move_out; }) fill(n.move_out, 81);
    if constexpr(requires { n.root; }) fill(n.root, 91);
    if constexpr(requires { n.elements; }) fill(n.elements, 101);
    if constexpr(requires { n.tuple; }) fill(n.tuple, 111);
    if constexpr(requires { n.length; }) fill(n.length, 121);
    if constexpr(requires { n.fields; }) fill(n.fields, 131);
    if constexpr(requires { n.descending; }) fill(n.descending, 141);
    if constexpr(requires { n.initialization_guard; }) fill(n.initialization_guard, 151);
    if constexpr(requires { n.bounds_guard; }) fill(n.bounds_guard, 161);
    if constexpr(requires { n.parallel_receipt; }) fill(n.parallel_receipt, 171);
    if constexpr(requires { n.bounds; }) fill(n.bounds, 181);
    if constexpr(requires { n.first_address; }) fill(n.first_address, 191);
    if constexpr(requires { n.payload; }) fill(n.payload, 201);
    if constexpr(requires { n.container; }) fill(n.container, 211);
    if constexpr(requires { n.start; }) fill(n.start, 221);
    if constexpr(requires { n.end; }) fill(n.end, 231);
    if constexpr(requires { n.step; }) fill(n.step, 241);
    if constexpr(requires { n.multiplier; }) fill(n.multiplier, 251);
    if constexpr(requires { n.native_fma; }) fill(n.native_fma, 261);
    if constexpr(requires { n.numerator; }) fill(n.numerator, 271);
    if constexpr(requires { n.denominator; }) fill(n.denominator, 281);
    if constexpr(requires { n.input; }) fill(n.input, 291);
    if constexpr(requires { n.inputs; }) fill(n.inputs, 301);
    if constexpr(requires { n.operand; }) fill(n.operand, 311);
    if constexpr(requires { n.left; }) fill(n.left, 321);
    if constexpr(requires { n.right; }) fill(n.right, 331);
    if constexpr(requires { n.re; }) fill(n.re, 341);
    if constexpr(requires { n.im; }) fill(n.im, 351);
    if constexpr(requires { n.text; }) fill(n.text, 361);
    if constexpr(requires { n.value_out; }) fill(n.value_out, 371);
    if constexpr(requires { n.ok_out; }) fill(n.ok_out, 381);
    if constexpr(requires { n.error_out; }) fill(n.error_out, 391);
    if constexpr(requires { n.shape; }) fill(n.shape, 401);
    if constexpr(requires { n.gpu; }) fill(n.gpu, 411);
    if constexpr(requires { n.tensor; }) fill(n.tensor, 421);
    if constexpr(requires { n.axis; }) fill(n.axis, 431);
    if constexpr(requires { n.indices; }) fill(n.indices, 441);
    if constexpr(requires { n.items; }) fill(n.items, 451);
    if constexpr(requires { n.target; }) fill(n.target, 461);
    if constexpr(requires { n.targets; }) fill(n.targets, 471);
    if constexpr(requires { n.autograd_targets; }) fill(n.autograd_targets, 481);
    if constexpr(requires { n.track; }) fill(n.track, 491);
    if constexpr(requires { n.extents; }) fill(n.extents, 501);
    if constexpr(requires { n.actual; }) fill(n.actual, 511);
    if constexpr(requires { n.expected; }) fill(n.expected, 521);
    if constexpr(requires { n.condition; }) fill(n.condition, 531);
    if constexpr(requires { n.current; }) fill(n.current, 541);
    if constexpr(requires { n.entry_array; }) fill(n.entry_array, 551);
    if constexpr(requires { n.entry_length; }) fill(n.entry_length, 561);
    if constexpr(requires { n.flag; }) fill(n.flag, 571);
    if constexpr(requires { n.primary; }) fill(n.primary, 581);
    if constexpr(requires { n.covered; }) fill(n.covered, 591);
    if constexpr(requires { n.holder_root; }) fill(n.holder_root, 601);
    if constexpr(requires { n.target_root; }) fill(n.target_root, 611);
    if constexpr(requires { n.destination_root; }) fill(n.destination_root, 621);
    if constexpr(requires { n.callee; }) fill(n.callee, 631);
    if constexpr(requires { n.args; }) fill(n.args, 641);
    if constexpr(requires { n.status; }) fill(n.status, 651);
    if constexpr(requires { n.error; }) fill(n.error, 661);
    if constexpr(requires { n.if_true; }) fill(n.if_true, 671);
    if constexpr(requires { n.if_false; }) fill(n.if_false, 681);
    if constexpr(requires { n.details; }) fill(n.details, 691);
    if constexpr(requires { n.cause; }) fill(n.cause, 701);
    if constexpr(requires { n.nodes; }) fill(n.nodes, 711);
    if constexpr(requires { n.iterations; }) fill(n.iterations, 721);
    if constexpr(requires { n.fill; }) fill(n.fill, 731);
    if constexpr(requires { n.bin; }) fill(n.bin, 741);
    if constexpr(requires { n.count; }) fill(n.count, 751);
    if constexpr(requires { n.initial; }) fill(n.initial, 761);
    if constexpr(requires { n.needle; }) fill(n.needle, 771);
    if constexpr(requires { n.prefix; }) fill(n.prefix, 781);
    if constexpr(requires { n.suffix; }) fill(n.suffix, 791);
    if constexpr(requires { n.separator; }) fill(n.separator, 801);
    if constexpr(requires { n.has_value; }) fill(n.has_value, 811);
    if constexpr(requires { n.cursor; }) fill(n.cursor, 821);
    if constexpr(requires { n.ok; }) fill(n.ok, 831);
    if constexpr(requires { n.values; }) fill(n.values, 841);
    if constexpr(requires { n.parts; }) fill(n.parts, 851);
    if constexpr(requires { n.added_length; }) fill(n.added_length, 861);
    if constexpr(requires { n.suffixes; }) fill(n.suffixes, 871);
    if constexpr(requires { n.default_value; }) fill(n.default_value, 881);
    if constexpr(requires { n.sync; }) fill(n.sync, 891);
    if constexpr(requires { n.seconds; }) fill(n.seconds, 901);
    if constexpr(requires { n.duration; }) fill(n.duration, 911);
    if constexpr(requires { n.program; }) fill(n.program, 921);
    if constexpr(requires { n.command; }) fill(n.command, 931);
    if constexpr(requires { n.path; }) fill(n.path, 941);
    if constexpr(requires { n.handle; }) fill(n.handle, 951);
    if constexpr(requires { n.position; }) fill(n.position, 961);
    if constexpr(std::is_same_v<T,FileHandleSeek>) fill(n.offset, 961);
    if constexpr(requires { n.source; }) fill(n.source, 971);
    if constexpr(requires { n.destination; }) fill(n.destination, 981);
    if constexpr(requires { n.recursive; }) fill(n.recursive, 991);
    if constexpr(requires { n.url; }) fill(n.url, 1001);
    if constexpr(requires { n.response; }) fill(n.response, 1011);
    if constexpr(requires { n.key; }) fill(n.key, 1021);
    if constexpr(requires { n.seed; }) fill(n.seed, 1031);
    if constexpr(requires { n.generator; }) fill(n.generator, 1041);
    if constexpr(requires { n.details; }) n.details={{811,"detail",false},{812,"other",false}};
    return n;
}
struct LegacyTrace : OperandVisitor {
    std::string trace;
    void definition(ValueId value) override {trace+="D"+std::to_string(value)+";";}
    void use(ValueId value) override {trace+="U"+std::to_string(value)+";";}
    void local(std::string_view name,LocalRole role) override {trace+="L"+std::to_string(name.size())+":"+std::string(name)+":"+std::to_string(unsigned(role))+";";}
    void successor(std::string_view name) override {trace+="S"+std::to_string(name.size())+":"+std::string(name)+";";}
};
struct TypedTrace final : LegacyTrace {
    using LegacyTrace::use;
    void use(ValueId value,OperandUse) override {LegacyTrace::use(value);}
};
constexpr std::array<std::string_view,231> legacy_expected{{
    "",
    "D11;",
    "D11;",
    "D11;",
    "D11;",
    "D11;",
    "D11;U101;U102;U101;",
    "D11;U121;",
    "D11;U131;U132;",
    "D11;U31;",
    "U31;U71;",
    "L6:name21:0;",
    "L6:name21:0;",
    "D11;L6:name21:3;",
    "D11;U31;",
    "D11;U41;U51;",
    "D11;U61;",
    "U61;U71;",
    "U61;L6:name21:2;",
    "D11;L6:name21:1;",
    "D11;L6:name21:5;",
    "U71;L6:name21:6;",
    "D11;U41;",
    "D11;U41;",
    "D11;U41;",
    "D11;U41;U141;",
    "D11;U41;",
    "D11;U361;U51;",
    "D11;U361;U51;",
    "D11;U361;U751;U761;",
    "D11;U361;",
    "D11;U361;",
    "D11;U361;U771;",
    "D11;U361;U781;",
    "D11;U361;U791;",
    "D11;U361;U771;",
    "D11;U361;U221;U231;",
    "D11;U361;",
    "D11;U361;U801;",
    "D11;U361;U801;",
    "D361;D811;U821;",
    "U821;",
    "D321;D331;D831;U361;",
    "D11;U361;",
    "D11;U741;",
    "D361;D831;D661;U41;",
    "D11;U361;",
    "D11;U841;U801;",
    "D11;U841;U842;U841;",
    "D11;U851;U852;U801;",
    "D11;D861;U361;U851;U852;U801;",
    "D11;U361;",
    "D11;U361;U871;U872;U871;",
    "D11;U751;U731;",
    "D11;U121;U731;",
    "D11;U741;",
    "D11;U741;U51;",
    "U741;U51;U71;",
    "D11;U741;U221;U231;",
    "D11;U361;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;",
    "D11;U41;",
    "D11;U401;U411;",
    "D11;U421;U411;",
    "D11;U421;U401;",
    "D11;U421;U0;U0;",
    "D11;U421;",
    "D11;U421;U441;U401;",
    "D11;U421;U441;U401;",
    "D11;U421;",
    "D11;U421;",
    "D11;U421;",
    "D11;U421;",
    "D11;U421;",
    "U421;",
    "D11;U421;",
    "D11;U421;U461;",
    "U421;U471;U472;U481;U491;",
    "D11;U421;",
    "D11;U421;",
    "U71;U501;U502;",
    "U511;U521;",
    "D11;U321;U331;",
    "D11;U321;U331;",
    "D11;U421;",
    "D11;U421;U451;U452;U453;U454;U455;U456;",
    "U421;U441;U442;U441;U71;",
    "D11;U361;",
    "D371;D381;D391;U361;",
    "D11;",
    "D11;U291;",
    "D11;U301;U302;U301;",
    "D11;U71;",
    "D11;",
    "D11;U21;U51;",
    "D11;U21;U51;U881;",
    "D11;U21;U881;",
    "D11;U21;",
    "",
    "D11;",
    "D11;U941;",
    "D11;U941;",
    "D11;U941;",
    "D11;U951;",
    "D11;U951;",
    "D11;U951;",
    "D11;U951;U361;",
    "D11;U951;",
    "D11;U951;U961;",
    "U951;",
    "D11;U941;",
    "D11;U941;",
    "D11;U941;U361;",
    "D11;U941;U741;",
    "D11;U941;",
    "D11;U941;",
    "D11;U941;",
    "D11;U971;U981;",
    "D11;U971;U981;",
    "D11;U941;",
    "D11;U941;U991;",
    "D11;U21;",
    "D11;U21;",
    "U531;",
    "D11;U891;",
    "D11;U221;U891;",
    "D11;U901;",
    "U911;",
    "U51;",
    "D11;",
    "D11;U461;",
    "U461;",
    "D11;U461;",
    "D11;U1031;",
    "D11;U1041;U221;U231;",
    "D11;U1041;",
    "D11;U1041;",
    "D11;U921;U641;",
    "D11;U931;",
    "D11;U361;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;U1021;",
    "D11;U71;U51;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;",
    "D11;U71;",
    "D11;U321;U331;",
    "D11;U1001;",
    "D11;U1011;U21;",
    "D11;U41;U51;U151;U161;",
    "U41;U51;U71;U151;U161;U171;",
    "D11;U71;",
    "D11;U71;",
    "U71;",
    "D11;U311;",
    "D11;U321;U331;",
    "D11;U71;",
    "D11;U71;",
    "D11;L6:name21:1;",
    "U71;L6:name21:2;",
    "D11;",
    "D11;U631;U641;U642;U641;",
    "D11;U641;U642;U643;U644;",
    "D11;U201;",
    "D11;U211;",
    "D11;U211;",
    "D11;U71;",
    "U71;",
    "",
    "D11;",
    "U651;",
    "U661;",
    "U241;",
    "U71;",
    "",
    "S7:name461;",
    "U531;S7:name671;S7:name681;",
    "U71;",
    "U71;",
    "U41;U551;U561;",
    "U571;",
    "D11;U811;U812;U701;",
    "D11;U661;",
    "D11;U661;",
    "D11;U661;",
    "D11;U661;",
    "D11;U211;U631;",
    "D11;U41;U181;U182;U183;",
    "U41;U181;U182;U183;U71;",
    "D11;U361;U181;U182;U183;",
    "D11;U741;U181;U182;U183;",
    "U581;U583;U581;U583;U591;U593;U591;U593;",
    "U71;L7:name571:4;",
    "U73;L7:name571:1;",
    "D11;U341;U351;",
    "D11;U71;",
    "D11;U221;U231;U241;",
    "D11;U251;U221;U241;U261;",
    "D11;U221;U231;U241;",
    "D11;U271;U281;",
    "D11;U251;U221;U241;",
    "D11;U271;U281;",
    "D11;U271;U281;",
    "D11;U121;U191;U181;U182;U183;",
    "D11;U711;U712;U713;U714;U721;U722;U723;U724;",
    "D11;L6:name21:3;",
    "D11;U101;U102;U101;",
    "D11;U111;",
    "D11;U61;",
    "U521;U541;",
    "D11;U361;",
    "U61;",
    "D11;U31;",
    "D11;U31;",
    "U31;U61;U91;",
    "D11;",
    "U601;U611;",
    "U71;U621;",
    "", // StatementBoundary
    "", // InlineScopeBegin
    "", // InlineScopeCleanup
    "", // InlineScopeEnd
    "U61;L6:name21:0;", // InlineBindAddress
    "U651;", // InlineExit
}};
static_assert(legacy_expected.size()==std::variant_size_v<Instruction>,
    "every Instruction alternative requires an explicit frozen operand sequence");
template<std::size_t...I> void every_legacy_alternative(std::index_sequence<I...>) {
    const auto check=[]<std::size_t Index>() {
        const Instruction instruction{populated<std::variant_alternative_t<Index,Instruction>>()};
        LegacyTrace old;TypedTrace typed;
        visit_operands(instruction,old);visit_operands(instruction,typed);
        if(old.trace!=legacy_expected[Index]||typed.trace!=legacy_expected[Index])
            throw std::runtime_error("operand sequence mismatch at alternative "+std::to_string(Index)+
                ": expected ["+std::string(legacy_expected[Index])+"], legacy ["+old.trace+
                "], typed ["+typed.trace+"]");
    };
    (check.template operator()<I>(),...);
}

}
int main(){try{sparse_occurrences();local_and_domain_roles();call_roles();nested_context();ssa_invalidity();scope_and_sentinels();every_legacy_alternative(std::make_index_sequence<std::variant_size_v<Instruction>>{});std::puts("sparse typed def-use and SSA verifier: ok");return 0;}
catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
