#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

using namespace quidra;

static Program parse(std::string source) {
    return Parser(Lexer(source).scan()).parse();
}

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "parser test failure: " << message << "\n";
        std::exit(1);
    }
}

static void reject(const std::string& source);

static void reject_code(const std::string& source, const std::string& code) {
    try {
        (void)parse(source);
    } catch (const CompileError& error) {
        if (error.diagnostic().code == code) return;
        std::cerr << "expected " << code << ", got " << error.diagnostic().code << ":\n" << source;
        std::exit(1);
    } catch (const CompileErrors& errors) {
        for (const auto& diagnostic : errors.diagnostics()) {
            if (diagnostic.code == code) return;
        }
        std::cerr << "expected " << code << ":\n" << source;
        std::exit(1);
    }
    std::cerr << "unexpected parser acceptance, expected " << code << ":\n" << source;
    std::exit(1);
}

static void reject_message(const std::string& source, const std::string& fragment) {
    try {
        (void)parse(source);
    } catch (const CompileError& error) {
        if (error.diagnostic().code == "PARSE_ERROR" &&
            error.diagnostic().message.find(fragment) != std::string::npos) return;
        std::cerr << "expected PARSE_ERROR '" << fragment << "', got "
                  << error.diagnostic().code << " '" << error.diagnostic().message << "':\n"
                  << source;
        std::exit(1);
    } catch (const CompileErrors& errors) {
        for (const auto& diagnostic : errors.diagnostics()) {
            if (diagnostic.code == "PARSE_ERROR" &&
                diagnostic.message.find(fragment) != std::string::npos) return;
        }
        std::cerr << "expected PARSE_ERROR '" << fragment << "':\n" << source;
        std::exit(1);
    }
    std::cerr << "unexpected parser acceptance, expected '" << fragment << "':\n" << source;
    std::exit(1);
}

static const IndexPart& slice_of(const Program& program, std::size_t statement) {
    const auto& binding = std::get<BindingStmt>(program.statements.at(statement)->data);
    return std::get<IndexExpr>(binding.value->data).items.at(0);
}

// Slice markers: a '<' or '>' that touches the first colon of a
// slice item marks its side exclusive; anywhere else it is a comparison.
static void slice_markers() {
    {
        auto marked = parse(
            "int[] x = values[0<:5]\n"
            "int[] y = values[10:>0:-2]\n"
            "int[] z = values[5>:0]\n"
            "int[] w = values[0:<5:2]\n"
            "int[] conflict = values[0<:>5]\n"
            "int[] spaced = values[0 <:5]\n"
            "int[] after = values[0<: 5]\n"
            "int[] plain = values[1:3]\n"
            "int[] open = values[:]\n"
            "int[] reverse = values[::-1]\n"
            "int[] tail = values[2:]\n"
            "int[] both = values[1<:<4]\n"
            "int[] down = values[4>:>1]\n"
        );
        const auto get = [&](std::size_t index) -> const IndexPart& {
            return slice_of(marked, index);
        };
        require(get(0).slice && get(0).start_form == SliceStart::exclusive_ascending &&
                    get(0).end_form == SliceEnd::inclusive,
                "ascending exclusive-start marker");
        require(get(1).slice && get(1).start_form == SliceStart::inclusive &&
                    get(1).end_form == SliceEnd::exclusive_descending && get(1).step,
                "descending exclusive-end marker and step");
        require(get(2).start_form == SliceStart::exclusive_descending &&
                    get(2).end_form == SliceEnd::inclusive,
                "descending exclusive-start marker");
        require(get(3).end_form == SliceEnd::exclusive_ascending && get(3).step,
                "ascending exclusive-end marker");
        require(get(4).start_form == SliceStart::exclusive_ascending &&
                    get(4).end_form == SliceEnd::exclusive_descending,
                "conflicting markers preserved for semantic diagnostics");
        // A marker belongs to the colon it touches; spaces on its other
        // side do not matter.
        require(get(5).slice && get(5).start &&
                    get(5).start_form == SliceStart::exclusive_ascending &&
                    get(5).end_form == SliceEnd::inclusive,
                "start marker after a space");
        require(get(6).slice && get(6).stop &&
                    get(6).start_form == SliceStart::exclusive_ascending &&
                    get(6).end_form == SliceEnd::inclusive,
                "start marker before a spaced end");
        require(get(7).start_form == SliceStart::inclusive && get(7).end_form == SliceEnd::inclusive,
                "an inclusive slice has no markers");
        require(get(8).slice && get(8).start_form == SliceStart::omitted &&
                    get(8).end_form == SliceEnd::omitted && !get(8).start && !get(8).stop,
                "omitted sides");
        require(get(9).start_form == SliceStart::omitted && get(9).end_form == SliceEnd::omitted &&
                    get(9).step,
                "a step alone");
        require(get(10).start_form == SliceStart::inclusive &&
                    get(10).end_form == SliceEnd::omitted,
                "an omitted end");
        require(get(11).start_form == SliceStart::exclusive_ascending &&
                    get(11).end_form == SliceEnd::exclusive_ascending,
                "exclusive on both sides");
        require(get(12).start_form == SliceStart::exclusive_descending &&
                    get(12).end_form == SliceEnd::exclusive_descending,
                "descending exclusive on both sides");
    }
    {
        // Comparisons that do not touch the colon, or that sit inside a
        // nested argument list or parentheses, stay comparisons.
        auto compared = parse(
            "int[] a = values[if x < y then 0 else 1:3]\n"
            "int[] b = values[f(x < 3):]\n"
            "int[] c = values[(x < 3):5]\n"
            "int[] d = values[if c then 0 else n>:0]\n"
            "int[] e = values[if c then n else m<:5]\n"
            "int[] g = values[make<int>(n)>:0]\n"
            "int[] h = values[x < y]\n"
            "int[] i = values[cut[1]<:4]\n"
        );
        const auto& a = slice_of(compared, 0);
        require(a.start_form == SliceStart::inclusive &&
                    std::holds_alternative<IfExpr>(a.start->data),
                "a comparison in an if-expression's condition");
        require(slice_of(compared, 1).start_form == SliceStart::inclusive,
                "a comparison inside a call");
        require(slice_of(compared, 2).start_form == SliceStart::inclusive,
                "a comparison inside parentheses");
        const auto& d = slice_of(compared, 3);
        require(d.start_form == SliceStart::exclusive_descending &&
                    std::holds_alternative<IfExpr>(d.start->data),
                "a marker after the final else branch marks the whole if-expression");
        require(slice_of(compared, 4).start_form == SliceStart::exclusive_ascending,
                "an ascending marker after the final else branch");
        const auto& g = slice_of(compared, 5);
        require(g.start_form == SliceStart::exclusive_descending &&
                    std::holds_alternative<CallExpr>(g.start->data),
                "a generic call before a start marker");
        const auto& h = slice_of(compared, 6);
        require(!h.slice && h.index && std::holds_alternative<BinaryExpr>(h.index->data),
                "an index comparison");
        require(slice_of(compared, 7).start_form == SliceStart::exclusive_ascending,
                "a nested index before a start marker");
    }
    const std::string spacing = "A slice marker is written next to its colon";
    reject_message("int[] x = values[0: <5]\n", spacing);
    reject_message("int[] x = values[0 < :5]\n", spacing);
    reject_message("int[] x = values[0: >5]\n", spacing);
    reject_message("int[] x = values[<:5]\n", "An exclusive start needs a start value");
    reject_message("int[] x = values[>:]\n", "An exclusive start needs a start value");
    reject_message("int[] x = values[2:<]\n", "An exclusive end needs an end value");
    reject_message("int[] x = values[2:>]\n", "An exclusive end needs an end value");
    reject_message("int[] x = values[:> ]\n", "An exclusive end needs an end value");
    reject_message("int[] x = values[0:5<:2]\n", "Slice markers belong to the first colon.");
    reject_message("int[] x = values[0:5:<2]\n", "Slice markers belong to the first colon.");
    reject_message("int[] x = values[0:5:>2]\n", "Slice markers belong to the first colon.");
    // Nested bounded ranges belong to their own expression, not the outer
    // index's slice syntax.
    {
        auto nested = parse("int[] x = values[if c then n>:0 else m]\n"
                            "int[] y = values[f(x<:3)]\n");
        const auto& conditional = slice_of(nested, 0);
        require(!conditional.slice &&
                    std::holds_alternative<IfExpr>(conditional.index->data),
                "a range in a then branch does not start an outer slice");
        const auto& call_index = slice_of(nested, 1);
        require(!call_index.slice &&
                    std::holds_alternative<CallExpr>(call_index.index->data),
                "a range argument does not start an outer slice");
    }
    // '<=' and '>=' are single tokens and never markers.
    reject("int[] x = values[s<=:e]\n");
    reject("int[] x = values[s>=:e]\n");
}

static const IfExpr& if_expression_of(const Program& program, std::size_t statement) {
    const auto& binding = std::get<BindingStmt>(program.statements.at(statement)->data);
    return std::get<IfExpr>(binding.value->data);
}

static void if_expressions() {
    {
        auto p = parse("int x = if ready then 1 else 2\n");
        const auto& node = if_expression_of(p, 0);
        require(node.conditions.size() == 1 && node.values.size() == 1 && node.otherwise,
                "if-expression AST");
    }
    {
        auto p = parse("string grade = if s >= 90 then \"A\" elif s >= 80 then \"B\" elif s >= 70 then \"C\" else \"D\"\n");
        const auto& node = if_expression_of(p, 0);
        require(node.conditions.size() == 3 && node.values.size() == 3,
                "elif chains of an if-expression");
    }
    {
        // A branch extends as far as an or_expr does.
        auto p = parse("int x = if c then 2 else 3 + 1\n");
        const auto& node = if_expression_of(p, 0);
        require(std::holds_alternative<BinaryExpr>(node.otherwise->data),
                "an if-expression branch is an or_expr");
    }
    {
        // Parenthesized nesting in every position, operands, and the
        // whole-expression positions.
        auto p = parse(
            "int a = if x then (if y then 1 else 2) else (if z then 3 else 4)\n"
            "int b = if (if x then y else z) then 1 else 2\n"
            "int c = 1 + (if x then 2 else 3)\n"
            "int d = (if x\n    then 1\n    else 2)\n"
            "f(if x then 1 else 2, named = if y then 3 else 4)\n"
            "int[] e = [if x then 1 else 2, 3]\n"
            "int g = e[if x then 0 else 1]\n"
            "g += if x then 1 else 2\n"
            "print(\"{if x then a else b}\")\n"
            "int h(int v = if x then 1 else 2)\n"
            "    return if v > 0 then v else 0\n"
            "class Box()\n"
            "    int v = if x then 1 else 2\n");
        require(p.statements.size() == 9 && p.functions.size() == 1 && p.classes.size() == 1,
                "if-expressions in whole-expression positions");
        require(std::holds_alternative<IfExpr>(
                    std::get<BindingStmt>(p.statements[3]->data).value->data),
                "a multi-line if-expression in parentheses");
    }
    reject_code("int x = if c then 1\n", "IF_EXPRESSION");
    reject_code("int x = if c then 1 else if d then 2 else 3\n", "IF_EXPRESSION");
    reject_code("int x = if c 1 else 2\n", "IF_EXPRESSION");
    reject_code("int x = if c then if d then 1 else 2 else 3\n", "IF_EXPRESSION");
    reject_code("int x = if if c then d else e then 1 else 2\n", "IF_EXPRESSION");
    reject_code("int x = 1 + if c then 2 else 3\n", "IF_EXPRESSION");
    reject_code("if if c then d else e\n    print(1)\n", "IF_EXPRESSION");
    reject_code("while if c then d else e\n    print(1)\n", "IF_EXPRESSION");
    reject_code("match if c then d else e\n    int v\n        print(v)\n", "IF_EXPRESSION");
    reject_code("if c then print(1) else print(2)\n", "IF_EXPRESSION");
    reject_code("if c then\n    print(1)\n", "IF_EXPRESSION");
    reject_code("print(\"{if c then 1}\")\n", "IF_EXPRESSION");
    // `then` is reserved, and there is no ?: operator.
    reject("int then = 1\n");
    reject("int x = c ? 1 : 2\n");
}

static void reject(const std::string& source) {
    try {
        (void)parse(source);
    } catch (const CompileError&) {
        return;
    } catch (const CompileErrors&) {
        return;
    }
    std::cerr << "unexpected parser acceptance:\n" << source;
    std::exit(1);
}

static const Expr& binding_value_of(const Program& program, std::size_t statement) {
    return *std::get<BindingStmt>(program.statements.at(statement)->data).value;
}

// Imaginary literals: a number immediately followed by `i` that does not
// start an identifier; `i` itself stays an ordinary name.
static void imaginary_literals() {
    {
        auto p = parse("com z = 2.0i\n");
        const auto* literal = std::get_if<ImaginaryLiteralExpr>(&binding_value_of(p, 0).data);
        require(literal && literal->spelling == "2.0" && !literal->integer_form,
                "2.0i is an imaginary literal");
    }
    {
        auto p = parse("com z = 1.0e5i\n");
        const auto* literal = std::get_if<ImaginaryLiteralExpr>(&binding_value_of(p, 0).data);
        require(literal && literal->spelling == "1.0e5" && !literal->integer_form,
                "1.0e5i is an imaginary literal");
    }
    {
        auto p = parse("com z = 2i\n");
        const auto* literal = std::get_if<ImaginaryLiteralExpr>(&binding_value_of(p, 0).data);
        require(literal && literal->spelling == "2" && literal->integer_form,
                "2i is an integer-form imaginary literal");
    }
    {
        const auto tokens = Lexer("2.0in 3.0i_x\n").scan();
        require(tokens.size() >= 4 && tokens[0].kind == TokenKind::RealLiteral &&
                    tokens[0].text == "2.0" && tokens[1].kind == TokenKind::Identifier &&
                    tokens[1].text == "in" && tokens[2].kind == TokenKind::RealLiteral &&
                    tokens[3].kind == TokenKind::Identifier && tokens[3].text == "i_x",
                "an i followed by an identifier character ends no imaginary literal");
    }
    {
        auto p = parse("int i = 5\nint j = i\n");
        require(std::holds_alternative<IntegerExpr>(binding_value_of(p, 0).data) &&
                    std::holds_alternative<NameExpr>(binding_value_of(p, 1).data),
                "i stays an identifier");
    }
    {
        auto p = parse("int in = 5\nint j = in\n");
        const auto* name = std::get_if<NameExpr>(&binding_value_of(p, 1).data);
        require(name && name->name == "in", "in is an ordinary identifier");
        reject("for int x in values\n    print(x)\n");
    }
}

// L17: brace shapes. Each axis is classified on tokens, so `n` (a bare name)
// and `(n)` (an expression) differ; a shape may span lines; braces appear
// nowhere else.
static void brace_shapes() {
    {
        auto p = parse("tensor<real32>{nat B, 3, nat H = 2 * B, n, (n), nat(n), len(v), _} x\n");
        const auto& type = std::get<BindingStmt>(p.statements.at(0)->data).declared_type;
        require(type.brace_shape && type.shape_axes.size() == 8 &&
                    type.tensor_rank && *type.tensor_rank == 8,
                "brace shape axes");
        const auto kind = [&](std::size_t axis) { return type.shape_axes.at(axis).kind; };
        require(kind(0) == ShapeAxisKind::Declare && type.shape_axes[0].name == "B" &&
                    kind(1) == ShapeAxisKind::Unnamed &&
                    kind(2) == ShapeAxisKind::DeclareEqual && type.shape_axes[2].name == "H" &&
                    type.shape_axes[2].value &&
                    kind(3) == ShapeAxisKind::Bare && type.shape_axes[3].name == "n" &&
                    kind(4) == ShapeAxisKind::Unnamed && kind(5) == ShapeAxisKind::Unnamed &&
                    kind(6) == ShapeAxisKind::Unnamed && kind(7) == ShapeAxisKind::Discard,
                "the axis classification");
        require(type.tensor_shape_prefix ==
                    std::vector<long long>({-1, 3, -2, -2, -2, -2, -2, -1}),
                "brace shape codes");
        require(type.tensor_shape_expressions.size() == 8 && type.tensor_shape_expressions[1] &&
                    !type.tensor_shape_expressions[0] && !type.tensor_shape_expressions[3],
                "only literal axes keep an extent expression");
    }
    {
        auto p = parse("tensor<real32>{nat B,\n    nat C} x = f()\nprint(1)\n");
        require(p.statements.size() == 2, "a brace shape across two lines is one statement");
    }
    {
        auto p = parse("tensor<real32>{if wide then 4 else 2} x\n");
        const auto& type = std::get<BindingStmt>(p.statements.at(0)->data).declared_type;
        require(std::holds_alternative<IfExpr>(type.shape_axes.at(0).value->data),
                "an if-expression is an unnamed axis");
    }
    {
        auto p = parse("tensor<real32>{2, 3} f(tensor<real32>{3, _}[] values)\n"
                       "    return values[0]\n"
                       "match v\n"
                       "    tensor<real32>{3, nat W} rgb\n"
                       "        print(W)\n"
                       "    map.Map<string, tensor<real32>{2}> m\n"
                       "        print(1)\n");
        require(p.functions.size() == 1 && p.statements.size() == 1,
                "brace shapes in results, parameters, arrays, match cases and type arguments");
    }
    {
        auto p = parse("print(\"{count<tensor<real32>{2, 3}>(xs)}\")\n"
                       "print(\"{first<tensor<real32>{2}>(xs):int=3}\")\n");
        require(p.statements.size() == 2, "braced type arguments in interpolations");
    }
    {
        auto p = parse("tensor<real32>{...} any\n"
                       "tensor<real32>{3, ...} head\n"
                       "tensor<real32>{..., nat C} tail\n"
                       "tensor<real32>{nat N, ..., N} ends\n");
        const auto type = [&](std::size_t i) -> const TypeName& {
            return std::get<BindingStmt>(p.statements.at(i)->data).declared_type;
        };
        require(type(0).tensor_shape_ellipsis == 0 && type(0).shape_axes.empty() &&
                    type(0).tensor_shape_prefix.empty() && !type(0).tensor_rank,
                "bare ellipsis stores a boundary and no placeholder axis");
        require(type(1).tensor_shape_ellipsis == 1 &&
                    type(1).tensor_shape_prefix == std::vector<long long>{3} &&
                    !type(1).tensor_rank,
                "leading ellipsis pattern does not infer exact rank");
        require(type(2).tensor_shape_ellipsis == 0 && type(2).shape_axes.size() == 1 &&
                    type(2).shape_axes[0].kind == ShapeAxisKind::Declare &&
                    type(2).shape_axes[0].name == "C",
                "trailing axes keep their declarations");
        require(type(3).tensor_shape_ellipsis == 1 && type(3).shape_axes.size() == 2 &&
                    type(3).tensor_shape_expressions.size() == 2 &&
                    type(3).shape_axes[1].kind == ShapeAxisKind::Bare,
                "ellipsis boundary separates explicit head and tail axes");
    }
    reject_code("tensor{2, 3} x = f()\n", "TENSOR_TYPE");
    reject_code("tensor<real32>{int N} x = f()\n", "SHAPE_VARIABLE");
    reject_code("int x = {3}\n", "PARSE_ERROR");
    reject_code("int x = 3}\n", "PARSE_ERROR");
    reject("tensor<real32>{} x\n");
    reject("tensor<real32>{3,} x\n");
    reject("tensor<real32>{3, , _} x\n");
    reject("tensor<real32>{..., ...} x\n");
    reject("tensor<real32>{..} x\n");
    reject("tensor<real32>{....} x\n");
    reject("tensor<real32>{... 3} x\n");
    reject("tensor<real32>{...,} x\n");
}

static void local_declarations() {
    auto program = parse("local const int outer = 1\nvoid use(local const int count, local int &target)\n    local int copy = count\n    target = copy\n");
    const auto& binding = std::get<BindingStmt>(program.statements[0]->data);
    require(binding.is_local && binding.is_const, "local const binding qualifiers");
    require(program.functions[0].parameters[0].is_local &&
            program.functions[0].parameters[0].is_const &&
            program.functions[0].parameters[1].is_local &&
            program.functions[0].parameters[1].writable,
            "local parameter qualifiers preserve value/reference contract");
    const auto& inner = std::get<BindingStmt>(program.functions[0].body[0]->data);
    require(inner.is_local, "function-body local binding flag");
    auto ordinary = parse("int count = 1\nvoid use(int count)\n    int copy = count\n");
    require(!std::get<BindingStmt>(ordinary.statements[0]->data).is_local &&
            !ordinary.functions[0].parameters[0].is_local,
            "local remains optional for ordinary bindings and parameters");
    auto pattern = parse("local (int count, _) = (1, string(\"x\"))\n");
    const auto& tuple = std::get<BindingStmt>(pattern.statements[0]->data);
    require(tuple.is_local && tuple.pattern, "local qualifies a destructuring declaration");
    reject("local (left, right) = pair\n");
    reject("const local int count = 1\n");
}

static void bounded_range_precedence() {
    const auto binding_value = [](const Program& program) -> const Expr& {
        return *std::get<BindingStmt>(program.statements.at(0)->data).value;
    };
    {
        auto program = parse("auto values = -3:0\n");
        const auto* range = std::get_if<CallExpr>(&binding_value(program).data);
        require(range && range->callee == "$std.range.generate",
                "a negative start belongs to the range, not a unary array operation");
        require(std::holds_alternative<UnaryExpr>(range->args[0].value->data),
                "the range start retains its negative sign");
    }
    for (const auto* source : {"auto values = :9 ^ 2\n",
                               "auto values = :-3 ^ 2\n",
                               "auto values = 3:0:-1 ^ 2\n",
                               "auto values = -3:0 ^ 2\n"}) {
        auto program = parse(source);
        const auto* power = std::get_if<BinaryExpr>(&binding_value(program).data);
        require(power && power->op == "^" &&
                    std::holds_alternative<CallExpr>(power->left->data),
                "power applies to the whole range after signed endpoints or steps");
    }
    {
        auto program = parse("auto value = -2 ^ 2\n");
        const auto* minus = std::get_if<UnaryExpr>(&binding_value(program).data);
        require(minus && std::holds_alternative<BinaryExpr>(minus->operand->data),
                "scalar power still binds more tightly than unary minus");
    }
    {
        auto program = parse("auto values = -(3:0)\n");
        const auto* minus = std::get_if<UnaryExpr>(&binding_value(program).data);
        require(minus && std::holds_alternative<CallExpr>(minus->operand->data),
                "parentheses allow unary minus on the generated array");
    }
}

int main() {
    bounded_range_precedence();
    {
        auto program=parse("parallel for 5 > int x >= 0 step -2\n    continue\n"
                           "parallel\n    int first = 1\n");
        const auto& loop=std::get<ForStmt>(program.statements.at(0)->data);
        require(loop.parallel && loop.range && loop.range->descending,"parallel prefix preserves integer range syntax");
        require(loop.parallel_span.start.line==1 && loop.parallel_span.start.column==1,"parallel prefix source origin");
        require(std::holds_alternative<ParallelStmt>(program.statements.at(1)->data),"parallel suite remains a separate construct");
    }
    {
        auto program=parse("parallel for int x = left[], int y = right[]\n    void\n");
        const auto& loop=std::get<ForStmt>(program.statements.at(0)->data);
        require(loop.parallel && loop.collections.size()==1,"parallel zip retains additional bindings");
    }

    {
        auto program=parse("for int price = prices[], auto quantity = quantities[], _ = ignored[]\n    print(price)\n");
        const auto& loop=std::get<ForStmt>(program.statements.at(0)->data);
        require(loop.name=="price" && loop.collections.size()==2,"zip stores every binding");
        require(loop.collections[0].name=="quantity" && loop.collections[0].is_auto &&
                loop.collections[1].name=="_","zip binding metadata");
        std::string order;
        visit_for_header(loop,[&](const Expr& source){order+=std::get<NameExpr>(source.data).name+";";});
        require(order=="prices;quantities;ignored;","zip header visitor is left to right");
    }
    reject_code("for int &x = &values[], int y = other[]\n    print(y)\n","FOR_HEADER");
    reject_code("for int x = values[nat i], int y = other[]\n    print(y)\n","FOR_HEADER");
    reject_code("for int x < 3, int y = other[]\n    print(y)\n","FOR_HEADER");
    brace_shapes();
    imaginary_literals();
    {
        auto enums = parse(
            "enum Token\n"
            "    Number(real64)\n"
            "    Name(string)\n"
            "    Plus\n"
            "    End\n"
            "Token token = Token.Number(3.0)\n"
            "match token\n"
            "    Token.Number(value)\n"
            "        print(value)\n"
            "    Token.Name(name)\n"
            "        print(name)\n"
            "    Token.Plus\n"
            "        void\n"
            "    Token.End\n"
            "        void\n"
        );
        require(enums.enums.size() == 1 && enums.enums[0].variants.size() == 4,
                "enum declaration AST");
        require(enums.enums[0].variants[0].payload.has_value() &&
                enums.enums[0].variants[0].payload->name == "real64",
                "enum payload AST");
        const auto& match = std::get<MatchStmt>(enums.statements[1]->data);
        require(match.cases[0].tag == "Token.Number" &&
                match.cases[0].binder == std::optional<std::string>("value"),
                "enum payload match AST");
        require(match.cases[2].tag.empty() && !match.cases[2].binder,
                "enum no-payload match AST");
    }
    {
        auto generic = parse(
            "T maximum<T: ordered>(T a, T b)\n"
            "    return a\n"
            "class Box<T: equatable>()\n"
            "    T value\n"
        );
        require(generic.functions.size() == 1 &&
                generic.functions[0].type_parameters == std::vector<std::string>({"T"}) &&
                generic.functions[0].type_constraints == std::vector<std::string>({"ordered"}),
                "generic function constraint AST");
        require(generic.classes.size() == 1 &&
                generic.classes[0].type_constraints == std::vector<std::string>({"equatable"}),
                "generic class constraint AST");
        reject("T bad<T:>(T value)\n    return value\n");
    }
    reject("tensor<real32, 3> invalid\n");
    reject("tensor<3, _, _> invalid\n");
    {
        // `_` stays an identifier token: a typed `_` parses as a declaration,
        // so the checker can name the rule that applies (DISCARD).
        auto discard = parse("int _ = 1\n");
        require(std::get<BindingStmt>(discard.statements[0]->data).name == "_",
                "a typed discard parses as a binding");
    }
    reject("tensor<real32>{3, , _} invalid\n");
    reject("tensor<real32>{3, _ ,} invalid\n");
    {
        auto shaped = parse(
            "tensor<real32>{3, _, _} image\n"
            "tensor<real64>{_, 768} wide\n"
        );
        const auto& image = std::get<BindingStmt>(shaped.statements[0]->data).declared_type;
        const auto& wide = std::get<BindingStmt>(shaped.statements[1]->data).declared_type;
        require(image.arguments.size() == 1 &&
                image.tensor_shape_prefix == std::vector<long long>({3, -1, -1}),
                "tensor exact shape pattern AST");
        require(wide.arguments.size() == 1 &&
                wide.tensor_shape_prefix == std::vector<long long>({-1, 768}),
                "tensor explicit dtype shape pattern AST");
    }
    {
        auto extents = parse(
            "int n = 3\n"
            "int m = 4\n"
            "tensor<real64>{nat(n * 2 + 1), _, 224} image\n"
            "tensor<real64>{nat(n * m), 224} graph\n"
            "real64[nat(n * m)] row\n"
            "real64[][nat(n * m)] nested\n"
        );
        const auto& image =
            std::get<BindingStmt>(extents.statements[2]->data).declared_type;
        const auto& graph =
            std::get<BindingStmt>(extents.statements[3]->data).declared_type;
        const auto& row =
            std::get<BindingStmt>(extents.statements[4]->data).declared_type;
        const auto& nested =
            std::get<BindingStmt>(extents.statements[5]->data).declared_type;
        require(image.tensor_shape_prefix ==
                    std::vector<long long>({-2, -1, 224}) &&
                image.shape_axes.size() == 3 &&
                image.shape_axes[0].value &&
                !image.shape_axes[1].value &&
                image.shape_axes[2].value,
                "tensor integer-expression shape AST");
        require(graph.tensor_shape_prefix ==
                    std::vector<long long>({-2, 224}) &&
                graph.shape_axes[0].value,
                "tensor graph integer-expression shape AST");
        require(row.dimensions == std::vector<long long>({-2}) &&
                row.dimension_expressions.size() == 1 &&
                row.dimension_expressions[0],
                "array integer-expression extent AST");
        require(nested.dimensions == std::vector<long long>({-1, -2}) &&
                nested.dimension_expressions.size() == 2 &&
                !nested.dimension_expressions[0] &&
                nested.dimension_expressions[1],
                "nested array captured extent AST");
    }

    {
        bool rejected = false;
        try {
            (void)parse(
                "int first =\n"
                "int second =\n"
            );
        } catch (const CompileErrors& errors) {
            rejected = true;
            require(errors.diagnostics().size() >= 2,
                    "multiple independent parser diagnostics");
            require(!errors.truncated(), "parser diagnostics should not truncate two errors");
        }
        require(rejected, "multiple parser error recovery");
    }
    {
        std::string nested_type;
        for (int i = 0; i < 256; ++i) nested_type += "Box<";
        nested_type += "int";
        nested_type.append(256, '>');

        bool declaration_depth = false;
        try {
            (void)parse(nested_type + " value\n");
        } catch (const CompileErrors& errors) {
            for (const auto& diagnostic : errors.diagnostics()) {
                if (diagnostic.code == "PARSE_DEPTH") declaration_depth = true;
            }
        }
        require(declaration_depth, "deep generic declaration depth guard");

        bool call_depth = false;
        try {
            (void)parse("print(factory<" + nested_type + ">())\n");
        } catch (const CompileErrors& errors) {
            for (const auto& diagnostic : errors.diagnostics()) {
                if (diagnostic.code == "PARSE_DEPTH") call_depth = true;
            }
        }
        require(call_depth, "deep generic call lookahead depth guard");
    }
    {
        std::string invalid = "string text = \"";
        invalid.push_back(static_cast<char>(0xc0));
        invalid.push_back(static_cast<char>(0xaf));
        invalid += "\"\n";
        bool rejected = false;
        try {
            (void)parse(invalid);
        } catch (const CompileError& error) {
            rejected = true;
            require(error.diagnostic().code == "INVALID_UTF8",
                    "invalid UTF-8 diagnostic code");
        }
        require(rejected, "invalid UTF-8 source rejection");
    }
    {
        auto lf = parse("string text = \"left\nright\"\n");
        auto crlf = parse("string text = \"left\r\nright\"\r\n");
        const auto& lf_binding = std::get<BindingStmt>(lf.statements[0]->data);
        const auto& crlf_binding = std::get<BindingStmt>(crlf.statements[0]->data);
        const auto& lf_text = std::get<StringExpr>(lf_binding.value->data).value;
        const auto& crlf_text = std::get<StringExpr>(crlf_binding.value->data).value;
        require(lf_text == "left\nright", "LF multiline string value");
        require(crlf_text == lf_text, "CRLF multiline string normalization");
        require(crlf_text.find('\r') == std::string::npos,
                "CRLF multiline string contains no carriage return");
    }
    {
        auto p = parse(
            "import geometry = \"./geometry.qui\"\n"
            "import root = \"@/shared/root.qui\"\n"
            "import plotting\n"
            "import plot = plotting\n"
        );
        require(p.imports.size() == 4, "imports");
        require(p.imports[0].alias == "geometry" && p.imports[0].target == "./geometry.qui" && p.imports[0].local_path,
                "relative local import");
        require(p.imports[1].target == "@/shared/root.qui" && p.imports[1].local_path,
                "command-root local import");
        require(p.imports[2].alias == "plotting" && p.imports[2].target == "plotting" && !p.imports[2].local_path,
                "package import");
        require(p.imports[3].alias == "plot" && p.imports[3].target == "plotting" && !p.imports[3].local_path,
                "package import alias");
    }

    {
        reject("import tensor += package\n");
    }

    {
        auto p = parse(
            "class Box<T>()\n"
            "    T value\n"
            "\n"
            "class Config()\n"
            "    int retries = 3\n"
            "    string endpoint\n"
            "\n"
            "T first<T>(T[] values)\n"
            "    return values[0]\n"
        );
        require(p.classes.size() == 2, "generic/default classes");
        require(p.classes[0].type_parameters.size() == 1 && p.classes[0].type_parameters[0] == "T",
                "generic class type parameter");
        require(p.classes[1].fields.size() == 2 &&
                p.classes[1].methods[0].is_constructor &&
                p.classes[1].methods[0].body.size() == 1 &&
                std::holds_alternative<AssignStmt>(p.classes[1].methods[0].body[0]->data),
                "ordered field initializer AST");
        require(p.functions.size() == 1 && p.functions[0].type_parameters.size() == 1,
                "generic function type parameter");
        require(p.functions[0].return_type.name == "T", "generic function result type");
    }

    {
        auto p = parse(
            "class Value()\n"
            "    int data\n"
            "\n"
            "class Holder()\n"
            "    Value value\n"
            "    int read()\n"
            "        return value.data\n"
        );
        require(p.classes.size() == 2, "composed classes");
        require(p.classes[1].fields.size() == 1 &&
                p.classes[1].fields[0].type.name == "Value",
                "class composition field");
    }

    {
        auto p = parse(
            "Box<int> a = Box<int>(value = 1)\n"
            "int x = first<int>([1, 2, 3])\n"
            "geometry.Point p\n"
        );
        require(p.statements.size() == 3, "generic and qualified statements");
        const auto& first_binding = std::get<BindingStmt>(p.statements[0]->data);
        require(first_binding.declared_type.name == "Box" &&
                first_binding.declared_type.arguments.size() == 1 &&
                first_binding.declared_type.arguments[0].name == "int",
                "generic declared type");
        const auto& ctor = std::get<CallExpr>(first_binding.value->data);
        require(ctor.type_arguments.size() == 1 && ctor.type_arguments[0].name == "int",
                "generic constructor call");
        const auto& qualified = std::get<BindingStmt>(p.statements[2]->data);
        require(qualified.declared_type.name == "geometry.Point", "qualified type");
    }

    {
        auto p = parse(
            "class Point()\n"
            "    int x\n"
            "    int y\n"
            "\n"
            "Point a = Point(x = 1, y = 2)\n"
            "Point b = Point(x = 1, y = 2)\n"
            "bool same = a == b\n"
        );
        require(p.statements.size() == 3, "class equality syntax");
        const auto& binding = std::get<BindingStmt>(p.statements[2]->data);
        const auto& eq = std::get<BinaryExpr>(binding.value->data);
        require(eq.op == "==", "class equality binary syntax");
    }

    {
        auto p = parse(
            "int x = 1\n"
            "int &y = &x\n"
            "print(&x)\n"
            "bool same = &x == &y\n"
        );
        require(p.statements.size() == 4, "address expression statement count");
        const auto& print_stmt = std::get<ExprStmt>(p.statements[2]->data);
        const auto& print_call = std::get<CallExpr>(print_stmt.value->data);
        require(!print_call.args[0].writable, "print address is a value expression");
        const auto& printed_address = std::get<UnaryExpr>(print_call.args[0].value->data);
        require(printed_address.op == "&", "print address unary syntax");
        const auto& same_binding = std::get<BindingStmt>(p.statements[3]->data);
        const auto& address_equal = std::get<BinaryExpr>(same_binding.value->data);
        require(address_equal.op == "==" &&
                std::get<UnaryExpr>(address_equal.left->data).op == "&" &&
                std::get<UnaryExpr>(address_equal.right->data).op == "&",
                "address equality syntax");
    }

    {
        auto p = parse(
            "void touch(int &value)\n"
            "    value = 1\n"
            "int x = 0\n"
            "touch(&x)\n"
        );
        const auto& call_stmt = std::get<ExprStmt>(p.statements[1]->data);
        const auto& call = std::get<CallExpr>(call_stmt.value->data);
        require(call.args[0].writable, "ordinary reference call keeps writable argument syntax");
    }

    {
        auto p = parse(
            "int x = 0\n"
            "if x < 0\n"
            "    print(\"negative\")\n"
            "elif x == 0\n"
            "    print(\"zero\")\n"
            "elif x == 1\n"
            "    print(\"one\")\n"
            "else\n"
            "    print(\"other\")\n"
        );
        require(p.statements.size() == 2, "elif statement count");
        const auto& root = std::get<IfStmt>(p.statements[1]->data);
        require(root.else_body.size() == 1, "elif first tail");
        const auto& second = std::get<IfStmt>(root.else_body[0]->data);
        require(second.else_body.size() == 1, "elif second tail");
        const auto& third = std::get<IfStmt>(second.else_body[0]->data);
        require(!third.else_body.empty(), "elif else tail");
    }

    {
        auto p = parse(
            "while true\n"
            "    continue\n"
            "    break\n"
        );
        const auto& loop = std::get<WhileStmt>(p.statements[0]->data);
        require(std::get<LoopControlStmt>(loop.body[0]->data).is_continue, "continue AST");
        require(!std::get<LoopControlStmt>(loop.body[1]->data).is_continue, "break AST");
    }

    {
        auto p = parse(
            "int x = 1\n"
            "x += 2\n"
            "x -= 3\n"
            "x *= 4\n"
            "x /= 5\n"
            "x %= 6\n"
        );
        require(p.statements.size() == 6, "compound assignment statement count");
        const char* expected[] = {"+", "-", "*", "/", "%"};
        for (std::size_t i = 1; i < p.statements.size(); ++i) {
            const auto& assignment = std::get<AssignStmt>(p.statements[i]->data);
            require(assignment.compound_op == expected[i - 1], "compound assignment operator");
        }
    }

    {
        auto p = parse(
            "void show()\n"
            "    string root = \"x/{ghost.field}\"\n"
        );
        const auto& binding = std::get<BindingStmt>(p.functions[0].body[0]->data);
        const auto& templ = std::get<StringTemplateExpr>(binding.value->data);
        const auto& member_expression = *templ.expressions[0];
        const auto& member = std::get<MemberExpr>(member_expression.data);
        const auto& name_expression = *member.base;
        const auto& name = std::get<NameExpr>(name_expression.data);
        require(name.name == "ghost", "interpolation member base");
        require(name_expression.span.start.line == 2 && name_expression.span.start.column == 23,
                "interpolation base start span");
        require(name_expression.span.end.line == 2 && name_expression.span.end.column == 28,
                "interpolation base end span");
        require(member_expression.span.start.line == 2 && member_expression.span.start.column == 23,
                "interpolation member start span");
        require(member_expression.span.end.line == 2 && member_expression.span.end.column == 34,
                "interpolation member end span");
    }

    {
        bool caught = false;
        try {
            (void)parse(
                "void show()\n"
                "    string root = \"x/{ghost.}\"\n"
            );
        } catch (const CompileError& error) {
            caught = true;
            require(error.diagnostic().span.start.line == 2 &&
                    error.diagnostic().span.start.column == 29,
                    "interpolation parse error span");
        } catch (const CompileErrors& errors) {
            caught = true;
            require(!errors.diagnostics().empty(), "interpolation aggregate diagnostic");
            require(errors.diagnostics()[0].span.start.line == 2 &&
                    errors.diagnostics()[0].span.start.column == 29,
                    "interpolation aggregate parse error span");
        }
        require(caught, "interpolation parse error");
    }

    {
        auto p = parse(
            "real64 value = 12.3456\n"
            "print(\"{value:int=5,frac=2,zero}\")\n"
            "print(\"{value:sig=4}\")\n"
        );
        const auto& first = std::get<ExprStmt>(p.statements[1]->data);
        const auto& call = std::get<CallExpr>(first.value->data);
        const auto& templ = std::get<StringTemplateExpr>(call.args[0].value->data);
        require(templ.formats.size() == 1, "interpolation format count");
        require(templ.formats[0].integer_width == 5 &&
                templ.formats[0].fractional_digits == 2 &&
                templ.formats[0].zero,
                "interpolation int frac zero format");

        const auto& second = std::get<ExprStmt>(p.statements[2]->data);
        const auto& sig_call = std::get<CallExpr>(second.value->data);
        const auto& sig_template = std::get<StringTemplateExpr>(sig_call.args[0].value->data);
        require(sig_template.formats[0].significant_digits == 4, "interpolation sig format");
    }

    reject("print(\"{1.2:frac=2,sig=3}\")\n");
    reject("print(\"{12:zero}\")\n");
    reject("print(\"{12:int=0}\")\n");
    reject("print(\"{12:precision=2}\")\n");

    reject(
        "void f()\n"
        "    import x = \"./x.qui\"\n"
    );
    {
        auto p = parse(
            "int super = 1\n"
            "int override = 2\n"
        );
        require(p.statements.size() == 2, "ordinary identifier spellings");
    }

    {
        // Bare members carry their own source span; self is explicit.
        auto p = parse(
            "class Box()\n"
            "    int size\n"
            "    int[] items\n"
            "    int twice()\n"
            "        items.append(size)\n"
            "        print(\"{size}\")\n"
            "        return size * 2\n"
        );
        const auto& method = p.classes[0].methods[1];
        const auto& append = std::get<MethodCallExpr>(
            std::get<ExprStmt>(method.body[0]->data).value->data);
        const auto& receiver = std::get<NameExpr>(append.receiver->data);
        require(receiver.name == "items" && receiver.this_qualifier &&
                receiver.this_qualifier->start.column == 9 &&
                receiver.this_qualifier->end.column == 14 &&
                append.receiver->span.start.column == 9,
                "bare member receiver AST and spans");
        const auto& argument = std::get<NameExpr>(append.args[0].value->data);
        require(argument.name == "size" && argument.this_qualifier, "bare member argument AST");
        const auto& print = std::get<CallExpr>(std::get<ExprStmt>(method.body[1]->data).value->data);
        const auto& text = std::get<StringTemplateExpr>(print.args[0].value->data);
        const auto& interpolated = std::get<NameExpr>(text.expressions[0]->data);
        require(interpolated.this_qualifier && text.expressions[0]->span.start.column == 17 &&
                interpolated.this_qualifier->start.column == 17,
                "bare member interpolation spans");
        auto bare = parse("int get(int size)\n    return size\n");
        const auto& ret = std::get<ReturnStmt>(bare.functions[0].body[0]->data);
        require(!std::get<NameExpr>(ret.value->data).this_qualifier, "ordinary parameter has no qualifier");
        auto shaped = parse(
            "class Grid()\n"
            "    int n\n"
            "    void make()\n"
            "        tensor<real32>{nat(n), 2} values = tensor.zeros<real32>([n, 2])\n"
        );
        const auto& binding = std::get<BindingStmt>(shaped.classes[0].methods[1].body[0]->data);
        require(binding.declared_type.shape_axes.size() == 2 &&
                binding.declared_type.shape_axes[0].value,
                "tensor shape expression is present");
        const auto* extent = std::get_if<CallExpr>(
            &binding.declared_type.shape_axes[0].value->data);
        require(extent && extent->callee == "nat" && extent->args.size() == 1 &&
                extent->args[0].value,
                "tensor shape contains a bare member");
        const auto* field = std::get_if<NameExpr>(&extent->args[0].value->data);
        require(field && field->name == "n" && field->this_qualifier,
                "bare member in a tensor shape");
        auto explicit_self = parse(
            "class Self this()\n"
            "    int size = 1\n"
            "    Self copy()\n"
            "        return this\n"
        );
        const auto& self_return = std::get<ReturnStmt>(explicit_self.classes[0].methods[1].body[0]->data);
        require(std::get<NameExpr>(self_return.value->data).self_reference,
                "the header declares the whole self identity");
        auto ordinary = parse("int this = 3\nclass this()\n    int value\n");
        require(ordinary.statements.size() == 1 && ordinary.classes.size() == 1,
                "this is an ordinary identifier");
        auto implicit = parse("class Box()\n    int size\n    int get()\n        return this\n");
        const auto& implicit_return = std::get<ReturnStmt>(implicit.classes[0].methods[1].body[0]->data);
        require(!std::get<NameExpr>(implicit_return.value->data).self_reference,
                "a header without a self name does not capture this");
    }
    reject_code("class Box\n    int value\n", "CONSTRUCTOR_SIGNATURE");
    reject_code("class Box self()\n    self = 1\n", "WRITE_CAPABILITY");

    local_declarations();

    slice_markers();

    if_expressions();

    {
        const auto source = std::string("int[] values = [1, 2]\nauto output = [\n    for int element = values[]\n    if element > 0\n    then element * element\n]\n");
        auto program = parse(source);
        const auto* binding = std::get_if<BindingStmt>(&program.statements[1]->data);
        require(binding && binding->value, "generation result binding");
        const auto* generated = std::get_if<ArrayComprehensionExpr>(&binding->value->data);
        require(generated && generated->type && generated->type->name == "int" &&
                generated->name == "element" && generated->condition && generated->source && generated->value,
                "generation source, typed binding, condition and value");
        const auto* source_name = std::get_if<NameExpr>(&generated->source->data);
        require(source_name && source_name->name == "values", "generation strips only the source suffix");
        require(generated->binding_span.start.line == 3 && generated->value->span.start.line == 5,
                "multiline generation preserves child spans");
        auto no_filter = parse("int[] values = []\nauto output = [for auto element = values[] then element]\n");
        const auto& plain = std::get<BindingStmt>(no_filter.statements[1]->data);
        const auto& generated_plain = std::get<ArrayComprehensionExpr>(plain.value->data);
        require(!generated_plain.condition && !generated_plain.type, "optional filter and inferred binding");
    }
    reject_code("int[] values = []\nauto output = [for int &x = &values[] then x]\n", "ARRAY_COMPREHENSION");
    reject_code("int[] values = []\nauto output = [for int x = &values[] then x]\n", "ARRAY_COMPREHENSION");
    reject_code("int[] values = []\nauto output = [for int x = values[], int y = values[] then x + y]\n", "ARRAY_COMPREHENSION");
    reject_code("auto output = [for 0 <= int x < 3 then x]\n", "ARRAY_COMPREHENSION");
    reject_code("int[] values = []\nauto output = [for int x = values[] x]\n", "ARRAY_COMPREHENSION");

    std::cout << "all parser tests passed\n";
}
