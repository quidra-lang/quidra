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

int main() {
    {
        auto enums = parse(
            "enum Token\n"
            "    Number(float)\n"
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
                enums.enums[0].variants[0].payload->name == "float",
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
            "class Box<T: equatable>\n"
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
    reject("tensor<float32, 3> invalid\n");
    reject("tensor<3, _, _> invalid\n");
    reject("tensor<float32><3, , _> invalid\n");
    reject("tensor<float32><3, _ ,> invalid\n");
    {
        auto shaped = parse(
            "tensor<float32><3, _, _> image\n"
            "neural<3, _, _> graph\n"
            "neural<float><_, 768> wide\n"
        );
        const auto& image = std::get<BindingStmt>(shaped.statements[0]->data).declared_type;
        const auto& graph = std::get<BindingStmt>(shaped.statements[1]->data).declared_type;
        const auto& wide = std::get<BindingStmt>(shaped.statements[2]->data).declared_type;
        require(image.arguments.size() == 1 &&
                image.tensor_shape_prefix == std::vector<long long>({3, -1, -1}),
                "tensor exact shape pattern AST");
        require(graph.arguments.empty() &&
                graph.tensor_shape_prefix == std::vector<long long>({3, -1, -1}),
                "neural default-float shape shorthand AST");
        require(wide.arguments.size() == 1 &&
                wide.tensor_shape_prefix == std::vector<long long>({-1, 768}),
                "neural explicit dtype shape pattern AST");
    }
    {
        auto extents = parse(
            "int n = 3\n"
            "int m = 4\n"
            "tensor<float><n * 2 + 1, _, 224> image\n"
            "neural<float><n * m, 224> graph\n"
            "float[n * m] row\n"
            "float[][n * m] nested\n"
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
                image.tensor_shape_expressions.size() == 3 &&
                image.tensor_shape_expressions[0] &&
                !image.tensor_shape_expressions[1] &&
                image.tensor_shape_expressions[2],
                "tensor integer-expression shape AST");
        require(graph.tensor_shape_prefix ==
                    std::vector<long long>({-2, 224}) &&
                graph.tensor_shape_expressions[0],
                "neural integer-expression shape AST");
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
        reject("import neural += package\n");
    }

    {
        auto p = parse(
            "class Box<T>\n"
            "    T value\n"
            "\n"
            "class Config\n"
            "    int retries = 3\n"
            "    string endpoint\n"
            "\n"
            "T first<T>(T[] values)\n"
            "    return values[0]\n"
        );
        require(p.classes.size() == 2, "generic/default classes");
        require(p.classes[0].type_parameters.size() == 1 && p.classes[0].type_parameters[0] == "T",
                "generic class type parameter");
        require(p.classes[1].fields.size() == 2 && p.classes[1].fields[0].default_value,
                "field default AST");
        require(p.functions.size() == 1 && p.functions[0].type_parameters.size() == 1,
                "generic function type parameter");
        require(p.functions[0].return_type.name == "T", "generic function result type");
    }

    {
        auto p = parse(
            "class Value\n"
            "    int data\n"
            "\n"
            "class Holder\n"
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
            "class Point\n"
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
            "float value = 12.3456\n"
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

    std::cout << "all parser tests passed\n";
}
