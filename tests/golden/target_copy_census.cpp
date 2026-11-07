#include "target_copy_census.hpp"
#include "range_constraint_traversal.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

namespace quidra::golden {
namespace {
using Origins=std::set<std::string>;
using Environment=std::map<std::string,Origins>;
void join(Origins& into,const Origins& from){into.insert(from.begin(),from.end());}
void join(Environment& into,const Environment& from){for(const auto& [name,origins]:from)join(into[name],origins);}
template<class Field> bool reference_field(const Field& field){
    if constexpr(requires{field.reference;})return field.reference;
    return false;
}
struct Summary {
    Origins returned,used;
    std::vector<Origins> outputs;
    bool operator==(const Summary&) const=default;
};
class Collector {
public:
    explicit Collector(const CheckedProgram& checked):checked_(checked) {
        for(const auto& function:checked.program.functions)functions_[function.name]=&function;
        for(const auto& declaration:checked.program.classes){
            std::size_t constructor=0;
            for(const auto& method:declaration.methods)functions_[method.is_constructor?
                member_function_name::constructor(declaration.name,constructor++):
                member_function_name::method(declaration.name,method.name)]=&method;
        }
    }
    std::vector<std::string> run(){
        // Every transfer is a union over a finite set of parameter and source
        // copy identities. Recursive calls therefore reach a fixed point.
        bool changed;
        do {
            changed=false;
            for(const auto& [name,function]:functions_){
                const auto signature=checked_.functions.find(name);
                if(signature==checked_.functions.end())continue;
                Environment env;Summary result;result.outputs.resize(signature->second.parameters.size());
                for(std::size_t i=0;i<signature->second.parameters.size();++i)
                    if(carries(signature->second.parameters[i].type))env[signature->second.parameters[i].name].insert(parameter(name,i));
                current_=&result;file_=function->source_file.empty()?checked_.program.root_source_file:function->source_file;
                for(const auto& input:function->parameters)
                    visit_parameter_range_bounds(input,[&](const Expr& bound){expression(bound,env);});
                visit_type_range_bounds(function->return_type,[&](const Expr& bound){expression(bound,env);});
                block(function->body,env);
                for(std::size_t i=0;i<result.outputs.size();++i)result.outputs[i]=env[signature->second.parameters[i].name];
                auto merged=summaries_[name];join(merged.returned,result.returned);join(merged.used,result.used);
                merged.outputs.resize(result.outputs.size());
                for(std::size_t i=0;i<result.outputs.size();++i)join(merged.outputs[i],result.outputs[i]);
                if(merged!=summaries_[name]){summaries_[name]=std::move(merged);changed=true;}
            }
        }while(changed);
        Summary root;current_=&root;file_=checked_.program.root_source_file;Environment env;
        block(checked_.program.statements,env);
        Origins used=root.used;
        for(const auto& [name,summary]:summaries_)join(used,summary.used);
        std::vector<std::string> result;
        for(const auto& identity:used)if(const auto found=sites_.find(identity);found!=sites_.end())result.push_back(found->second);
        std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
    }
private:
    std::string parameter(const std::string& name,std::size_t index) const{return "parameter:"+name+":"+std::to_string(index);}
    const Type* type(const Expr& expression) const{
        const auto found=checked_.raw_types.find(&expression);return found==checked_.raw_types.end()?nullptr:&found->second;
    }
    bool holds(const Type& type,std::unordered_set<std::string>& active,bool references=false) const{
        if(type.kind==TypeKind::Class&&type.class_name==standard_class::autograd_target)return true;
        if(type.kind==TypeKind::Array||type.kind==TypeKind::Traversal)return type.first&&holds(*type.first,active,references);
        const auto parts=type.kind==TypeKind::Tuple?&type.parameters:type.kind==TypeKind::Union?&type.cases:nullptr;
        if(parts){for(const auto& child:*parts)if(holds(child,active,references))return true;return false;}
        if(type.kind!=TypeKind::Class||!active.insert(type.class_name).second)return false;
        bool found=false;
        if(const auto info=checked_.classes.find(type.class_name);info!=checked_.classes.end())
            for(const auto& field:info->second.fields)if(references||!reference_field(field))found=found||holds(field.type,active,references);
        active.erase(type.class_name);return found;
    }
    bool holds(const Type& type) const{std::unordered_set<std::string> active;return holds(type,active);}
    bool carries(const Type& type) const{std::unordered_set<std::string> active;return holds(type,active,true);}
    bool carries(const Expr& expression) const{const auto value=type(expression);return value&&carries(*value);}
    bool holds(const Expr& expression) const{const auto value=type(expression);return value&&holds(*value);}
    std::string root(const Expr& expression) const{
        if(const auto* name=std::get_if<NameExpr>(&expression.data))return name->this_qualifier?"$receiver":name->name;
        if(const auto* member=std::get_if<MemberExpr>(&expression.data))return root(*member->base);
        if(const auto* index=std::get_if<IndexExpr>(&expression.data))return root(*index->base);
        if(const auto* projection=std::get_if<TupleProjectionExpr>(&expression.data))return projection->subject;
        return {};
    }
    Origins copy(const Expr& expression,Origins value){
        // Checked projections move leaves from the once-captured pattern subject.
        // Its written source already identifies the copy; unpacking adds none.
        if(holds(expression)&&!root(expression).empty()&&!std::holds_alternative<TupleProjectionExpr>(expression.data)){
            const auto site=(file_.empty()?"-":file_)+":"+std::to_string(expression.span.start.line)+":"+std::to_string(expression.span.start.column);
            const auto identity="copy:"+site;sites_[identity]=site;value.insert(identity);
        }
        return value;
    }
    Origins substitute(const Origins& origins,const std::string& target,const std::vector<Origins>& arguments) const{
        Origins result;
        for(const auto& origin:origins){
            bool replaced=false;
            for(std::size_t i=0;i<arguments.size();++i)if(origin==parameter(target,i)){join(result,arguments[i]);replaced=true;break;}
            if(!replaced)result.insert(origin);
        }
        return result;
    }
    Origins expression(const Expr& value,Environment& env){
        Origins result;
        const auto eval=[&](const ExprPtr& child){return child?expression(*child,env):Origins{};};
        if(const auto* name=std::get_if<NameExpr>(&value.data))result=env[name->this_qualifier?"$receiver":name->name];
        else if(const auto* projection=std::get_if<TupleProjectionExpr>(&value.data))result=env[projection->subject];
        else if(const auto* member=std::get_if<MemberExpr>(&value.data))result=eval(member->base);
        else if(const auto* index=std::get_if<IndexExpr>(&value.data)){
            result=eval(index->base);for(const auto& item:index->items){eval(item.index);eval(item.start);eval(item.stop);eval(item.step);}
        }else if(const auto* tuple=std::get_if<TupleExpr>(&value.data)){
            for(const auto& element:tuple->elements)join(result,copy(*element,eval(element)));
        }else if(const auto* generation=std::get_if<ArrayComprehensionExpr>(&value.data)){
            auto source=eval(generation->source);auto before=env;
            env[generation->name]=source;
            if(generation->condition)eval(generation->condition);
            join(result,copy(*generation->value,eval(generation->value)));
            env=std::move(before);
        }else if(const auto* array=std::get_if<ArrayExpr>(&value.data)){
            for(const auto& element:array->elements)join(result,copy(*element,eval(element)));
        }else if(const auto* unary=std::get_if<UnaryExpr>(&value.data))result=eval(unary->operand);
        else if(const auto* attempt=std::get_if<TryExpr>(&value.data))result=eval(attempt->value);
        else if(const auto* binary=std::get_if<BinaryExpr>(&value.data)){result=eval(binary->left);join(result,eval(binary->right));}
        else if(const auto* chain=std::get_if<ComparisonChainExpr>(&value.data)){for(const auto& child:chain->operands)eval(child);}
        else if(const auto* text=std::get_if<StringTemplateExpr>(&value.data)){for(const auto& child:text->expressions)eval(child);}
        else if(const auto* choice=std::get_if<IfExpr>(&value.data)){
            for(const auto& condition:choice->conditions)eval(condition);
            for(const auto& branch:choice->values)join(result,copy(*branch,eval(branch)));
            if(choice->otherwise)join(result,copy(*choice->otherwise,eval(choice->otherwise)));
        }else if(std::holds_alternative<CallExpr>(value.data)||std::holds_alternative<MethodCallExpr>(value.data))result=call(value,env);
        return carries(value)?result:Origins{};
    }
    Origins call(const Expr& value,Environment& env){
        const auto* plain=std::get_if<CallExpr>(&value.data);const auto* method=std::get_if<MethodCallExpr>(&value.data);
        const auto& supplied=method?method->args:plain->args;
        const auto resolved_call=checked_.call_resolutions.find(&value);
        const bool constructor=resolved_call!=checked_.call_resolutions.end()&&resolved_call->second.kind==CallKind::Constructor;
        Origins receiver=method?expression(*method->receiver,env):constructor?Origins{}:env["$receiver"];
        std::vector<Origins> supplied_values;for(const auto& argument:supplied){
            auto origins=expression(*argument.value,env);supplied_values.push_back(argument.writable?std::move(origins):copy(*argument.value,std::move(origins)));
        }
        if(method){
            const auto receiver_type=type(*method->receiver);
            if(receiver_type&&receiver_type->kind==TypeKind::Tensor&&(method->method=="backward"||method->method=="track"))
                for(std::size_t i=0;i<supplied.size();++i)if(supplied[i].writable)join(current_->used,supplied_values[i]);
            if(receiver_type&&receiver_type->kind==TypeKind::Class&&receiver_type->class_name==standard_class::autograd_target&&
                (method->method=="has_grad"||method->method=="gradient"||method->method=="clear_grad"))join(current_->used,receiver);
        }
        std::string target;
        if(const auto resolved=checked_.method_calls.find(&value);resolved!=checked_.method_calls.end())target=resolved->second.internal_name;
        else if(const auto resolved=checked_.call_resolutions.find(&value);resolved!=checked_.call_resolutions.end())target=resolved->second.target;
        const auto signature=checked_.functions.find(target);
        if(signature==checked_.functions.end()){
            Origins result;for(const auto& item:supplied_values)join(result,item);join(result,receiver);
            if(plain&&plain->callee=="$std.reflect.collect"&&holds(value)){
                const auto site=(file_.empty()?"-":file_)+":"+std::to_string(value.span.start.line)+":"+std::to_string(value.span.start.column);
                const auto identity="copy:"+site;sites_[identity]=site;result.insert(identity);
            }
            return result;
        }
        const auto& parameters=signature->second.parameters;
        const bool member=!parameters.empty()&&parameters.front().name=="$receiver";
        std::vector<Origins> actual(parameters.size());std::vector<const Expr*> destinations(parameters.size(),nullptr);
        if(member){actual[0]=receiver;if(method)destinations[0]=method->receiver.get();}
        std::size_t positional=member?1:0;
        for(std::size_t i=0;i<supplied.size();++i){
            auto index=positional++;
            if(supplied[i].name){const auto named=std::find_if(parameters.begin(),parameters.end(),[&](const auto& parameter){return parameter.name==*supplied[i].name;});index=static_cast<std::size_t>(named-parameters.begin());}
            if(index>=actual.size())continue;
            actual[index]=supplied_values[i];destinations[index]=supplied[i].value.get();
        }
        const auto found=summaries_.find(target);
        if(found==summaries_.end()){
            Origins result;for(const auto& item:actual)join(result,item);
            if(functions_.contains(target)&&functions_.at(target)->external_symbol)join(current_->used,result);
            return result;
        }
        const auto& summary=found->second;join(current_->used,substitute(summary.used,target,actual));
        Origins result=substitute(summary.returned,target,actual);
        for(std::size_t i=0;i<summary.outputs.size();++i){
            const auto output=substitute(summary.outputs[i],target,actual);
            if(member&&i==0)join(result,output);
            if((parameters[i].writable||(member&&i==0))&&destinations[i]){
                const auto name=root(*destinations[i]);if(!name.empty())join(env[name],output);
            }else if(member&&i==0&&!method)join(env["$receiver"],output);
        }
        // Constructors can place copied inputs in a result's value fields.
        if(member&&plain&&holds(value))for(const auto& item:actual)join(result,item);
        return result;
    }
    void pattern(const PatternExpansion& expansion,const Origins& origins,Environment& env){
        env[expansion.subject]=origins;
        block(expansion.bindings,env);
    }
    void block(const std::vector<StmtPtr>& body,Environment& env){for(const auto& statement:body)walk(*statement,env);}
    void walk(const Stmt& statement,Environment& env){
        const auto eval=[&](const ExprPtr& child){return child?expression(*child,env):Origins{};};
        visit_statement_range_bounds(statement,[&](const Expr& bound){expression(bound,env);});
        if(const auto* binding=std::get_if<BindingStmt>(&statement.data)){
            auto origins=eval(binding->value);
            if(binding->value&&!binding->reference&&!binding->reference_initializer)origins=copy(*binding->value,std::move(origins));
            if(binding->pattern){
                if(const auto found=checked_.pattern_expansions.find(&statement);found!=checked_.pattern_expansions.end())pattern(found->second,origins,env);
            }else env[binding->name]=std::move(origins);
        }else if(const auto* assignment=std::get_if<AssignStmt>(&statement.data)){
            if(!assignment->pattern&&assignment->target_expr())expression(*assignment->target_expr(),env);
            auto origins=eval(assignment->value);
            if(assignment->compound_op.empty())origins=copy(*assignment->value,std::move(origins));
            if(assignment->pattern){
                if(const auto found=checked_.pattern_expansions.find(&statement);found!=checked_.pattern_expansions.end())pattern(found->second,origins,env);
            }else if(assignment->target_expr()){
                const auto name=root(*assignment->target_expr());if(!name.empty())join(env[name],origins);
            }
        }else if(const auto* binding=std::get_if<RebindStmt>(&statement.data))env[binding->name]=eval(binding->target);
        else if(const auto* returned=std::get_if<ReturnStmt>(&statement.data)){
            auto origins=eval(returned->value);if(returned->value)origins=copy(*returned->value,std::move(origins));join(current_->returned,origins);
        }else if(const auto* item=std::get_if<ExprStmt>(&statement.data))eval(item->value);
        else if(const auto* conditional=std::get_if<IfStmt>(&statement.data)){
            eval(conditional->condition);auto left=env,right=env;block(conditional->then_body,left);block(conditional->else_body,right);join(left,right);env=std::move(left);
        }else if(const auto* loop=std::get_if<WhileStmt>(&statement.data)){
            Environment before;do{before=env;eval(loop->condition);block(loop->body,env);join(env,before);}while(env!=before);
        }else if(const auto* loop=std::get_if<ForStmt>(&statement.data)){
            auto origins=eval(loop->iterable);
            std::vector<std::pair<std::string,Origins>> collection_origins;
            for(const auto& binding:loop->collections) {
                auto copied=eval(binding.iterable);
                const auto iterable=type(*binding.iterable);
                if(iterable&&iterable->first&&holds(*iterable->first)) {
                    const auto site=(file_.empty()?"-":file_)+":"+std::to_string(binding.iterable->span.start.line)+":"+std::to_string(binding.iterable->span.start.column);
                    const auto identity="copy:"+site;sites_[identity]=site;copied.insert(identity);
                }
                collection_origins.emplace_back(binding.name,std::move(copied));
            }
            if(loop->range){eval(loop->range->end);eval(loop->range->step);}
            const auto iterable=type(*loop->iterable);
            if(!loop->writable&&iterable&&iterable->first&&(iterable->kind==TypeKind::Array||iterable->kind==TypeKind::Traversal)&&holds(*iterable->first)){
                const auto site=(file_.empty()?"-":file_)+":"+std::to_string(loop->iterable->span.start.line)+":"+std::to_string(loop->iterable->span.start.column);
                const auto identity="copy:"+site;sites_[identity]=site;origins.insert(identity);
            }
            Environment before;do{
                before=env;
                if(loop->pattern){
                    if(const auto found=checked_.control_patterns.find(&*loop->pattern);found!=checked_.control_patterns.end())pattern(found->second,origins,env);
                }else env[loop->name]=origins;
                for(const auto& [name,copied]:collection_origins) if(name!="_") env[name]=copied;
                block(loop->body,env);join(env,before);
            }while(env!=before);
        }else if(const auto* match=std::get_if<MatchStmt>(&statement.data)){
            auto origins=eval(match->value);auto before=env;for(const auto& alternative:match->cases){auto branch=before;
                visit_type_range_bounds(alternative.type,[&](const Expr& bound){expression(bound,branch);});
                if(alternative.pattern){
                    if(const auto found=checked_.control_patterns.find(&*alternative.pattern);found!=checked_.control_patterns.end())pattern(found->second,origins,branch);
                }else if(alternative.binder)branch[*alternative.binder]=origins;
                block(alternative.body,branch);join(env,branch);}
        }else if(const auto* parallel=std::get_if<ParallelStmt>(&statement.data))block(parallel->body,env);
        else if(const auto* guard=std::get_if<MainGuardStmt>(&statement.data)){
            const auto file=file_;if(!guard->source_file.empty())file_=guard->source_file;block(guard->body,env);file_=file;
        }
    }
    const CheckedProgram& checked_;
    std::map<std::string,const FunctionDecl*> functions_;
    std::map<std::string,Summary> summaries_;
    std::map<std::string,std::string> sites_;
    Summary* current_{};std::string file_;
};
}
std::vector<std::string> target_copy_sites(const CheckedProgram& checked){return Collector(checked).run();}
}
