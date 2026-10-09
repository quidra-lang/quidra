#!/usr/bin/env python3
"""Gate Gseq: no unsequenced name allocation or emission (tests/golden/README.md,
Gates).

  check_sequencing.py [--build DIR] [FILE...]
  check_sequencing.py --self-test

The order in which the backend and the lowering allocate names (`temp`,
`label`, `fresh`, string interning, ...) and append text is part of the
output. C++ leaves the order of function arguments, of constructor arguments
in parentheses, and of the operands of most binary operators unspecified
(Clang evaluates arguments left to right, GCC and MSVC usually right to left),
so two effectful sub-expressions in one of those places can produce different
LLVM text on different compilers.

The lint parses every FILE with libclang and flags:
  - a call or parenthesized construct expression with two or more effectful
    arguments,
  - an unsequenced binary operator (anything except << >> && || , = op= and
    subscript, which C++17 sequences) with two or more effectful operands.
Braced initializers are sequenced left to right and are not flagged.

The effectful set is derived from the code, transitively, over all FILEs at
once (so a call into another translation unit counts):
  - a function is effectful when its body (lambda bodies included) changes
    state that outlives the call: it assigns, increments or decrements a data
    member, a global or static, or an object reached through a reference or
    pointer; or it calls a mutating standard-library member (push_back,
    insert, append, operator<<, a map's operator[], ...) on such an object or
    passes one to a standard function by non-const reference;
  - a function is effectful when it calls an effectful function, uses an
    effectful function or lambda variable as a value, or makes a call that
    cannot be resolved (a dependent name) to a name some effectful function
    has; this is iterated to a fixpoint;
  - a project function declared but defined in none of the FILEs is effectful
    when it is a non-const, non-static member function or takes a non-const
    reference (nothing is known about its body);
  - a virtual function is effectful when any function of the same name is.
No class names are listed: new emitters, services, lowering units and
scaffolding classes are covered as soon as they mutate state or call
something that does. An argument is effectful when, outside a lambda body, it
calls or names an effectful function or mutates such state itself.

Default FILEs: every .cpp under src/{llvm_backend,lowering,optimizer,ir,
llvm_text}. The Python bindings of libclang are found next to Homebrew/system
LLVM, or through LIBCLANG_PYTHON and LIBCLANG_LIBRARY. --self-test runs the
lint on a probe translation unit and checks the findings it must and must not
produce.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
SEQUENCED = {"<<", ">>", "&&", "||", ",", "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
             "^=", "<<=", ">>=", "[]"}
ASSIGNMENTS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
# Standard-library members that change their object.
STD_MUTATORS = {
    "push_back", "emplace_back", "push_front", "emplace_front", "pop_back", "pop_front",
    "emplace", "emplace_hint", "insert", "insert_or_assign", "try_emplace", "erase", "clear",
    "resize", "reserve", "shrink_to_fit", "assign", "append", "swap", "merge", "extract",
    "splice", "remove", "remove_if", "unique", "sort", "reverse", "put", "write", "seekp",
    "seekg", "flush", "getline", "read", "ignore", "reset", "release", "exchange", "store",
    "fetch_add", "fetch_sub", "operator=", "operator+=", "operator-=", "operator<<",
    "operator>>", "operator++", "operator--", "str", "setstate", "imbue", "rdbuf", "fill",
    "precision", "width", "setf", "unsetf",
}
MAP_TYPES = re.compile(r"\b(unordered_map|map|unordered_multimap|multimap)\b")
# The stateful name allocators, whose call order is part of the output; each
# must come out effectful.
# Backend: TemporaryNames::value and ::label (one counter; temp and
# unique_label forward to them), ScratchPlanner::reserve and ::reserve_shared,
# StringPool::intern. Lowerer: fresh, label, hidden, bind_source_local. The
# names are unqualified: a name passes when one function of that name is
# effectful (SymbolTable::value, which allocates nothing, shares "value").
# (The optimizer's next_rewrite_value scans the function and keeps no state:
# statements, not argument order, fix when it runs.)
NAME_ALLOCATORS = ("temp", "unique_label", "value", "label", "reserve", "reserve_shared",
                   "intern", "fresh", "hidden", "bind_source_local")


def import_clang():
    candidates = [os.environ.get("LIBCLANG_PYTHON", "")]
    for prefix in ("/opt/homebrew/opt/llvm/lib", "/usr/local/opt/llvm/lib", "/usr/lib/llvm-18/lib",
                   "/usr/lib/llvm-17/lib", "/usr/lib/llvm-16/lib", "/usr/lib/llvm-15/lib"):
        candidates += sorted(glob.glob(f"{prefix}/python3*/site-packages"), reverse=True)
        candidates += sorted(glob.glob(f"{prefix}/python3/dist-packages"))
    for path in candidates:
        if path and Path(path, "clang", "cindex.py").exists():
            sys.path.insert(0, path)
            break
    import clang.cindex as cindex  # noqa: E402
    library = os.environ.get("LIBCLANG_LIBRARY")
    if not library:
        for candidate in ("/opt/homebrew/opt/llvm/lib/libclang.dylib",
                          "/usr/local/opt/llvm/lib/libclang.dylib",
                          *sorted(glob.glob("/usr/lib/llvm-*/lib/libclang.so*"), reverse=True)):
            if Path(candidate).exists():
                library = candidate
                break
    if library:
        cindex.Config.set_library_file(library)
    return cindex, library


def resource_dir(library: str | None) -> str | None:
    """The builtin headers (stdarg.h, ...) of the clang that libclang belongs to."""
    if not library:
        return None
    clang = Path(library).resolve().parent.parent / "bin" / "clang"
    if not clang.exists():
        return None
    return os.popen(f"'{clang}' -print-resource-dir").read().strip() or None


def default_files() -> list[str]:
    files: list[str] = []
    for directory in ("llvm_backend", "lowering", "optimizer", "ir", "llvm_text"):
        files += sorted(str(p.relative_to(REPO)) for p in (REPO / "src" / directory).rglob("*.cpp"))
    return files


@dataclass
class Facts:
    """What an expression or a body does: the project functions it calls or
    names (USRs), unresolved callee names, and whether it changes state that
    outlives the call by itself."""
    usrs: set[str] = field(default_factory=set)
    names: set[str] = field(default_factory=set)
    mutates: bool = False

    def add(self, other: "Facts") -> None:
        self.usrs |= other.usrs
        self.names |= other.names
        self.mutates = self.mutates or other.mutates


@dataclass
class Function:
    name: str
    facts: Facts = field(default_factory=Facts)
    defined: bool = False
    conservative: bool = False  # could mutate its object or arguments
    virtual: bool = False


@dataclass
class Site:
    location: str
    message: str
    operands: list[Facts]


class Lint:
    FUNCTION_KINDS = ("FUNCTION_DECL", "CXX_METHOD", "CONSTRUCTOR", "DESTRUCTOR",
                      "FUNCTION_TEMPLATE", "CONVERSION_FUNCTION")
    SCOPE_KINDS = ("NAMESPACE", "CLASS_DECL", "STRUCT_DECL", "CLASS_TEMPLATE",
                   "CLASS_TEMPLATE_PARTIAL_SPECIALIZATION", "UNEXPOSED_DECL", "LINKAGE_SPEC")
    CAST_KINDS = ("UNEXPOSED_EXPR", "PAREN_EXPR", "CSTYLE_CAST_EXPR", "CXX_STATIC_CAST_EXPR",
                  "CXX_CONST_CAST_EXPR", "CXX_REINTERPRET_CAST_EXPR", "CXX_FUNCTIONAL_CAST_EXPR")

    def __init__(self, cindex, roots: list[Path]):
        self.cindex = cindex
        self.K = cindex.CursorKind
        self.T = cindex.TypeKind
        self.roots = [str(root.resolve()) for root in roots]
        self.functions: dict[str, Function] = {}
        self.sites: list[Site] = []
        self.calls_checked = 0
        self.function_kinds = {getattr(self.K, k) for k in self.FUNCTION_KINDS if hasattr(self.K, k)}
        self.scope_kinds = {getattr(self.K, k) for k in self.SCOPE_KINDS if hasattr(self.K, k)}
        self.cast_kinds = {getattr(self.K, k) for k in self.CAST_KINDS if hasattr(self.K, k)}
        self.lambda_variables: set[str] = set()
        self.project_files: dict[str, bool] = {}

    # -- locations -------------------------------------------------------

    def in_project(self, cursor) -> bool:
        location = cursor.location
        if location.file is None:
            return False
        name = location.file.name
        known = self.project_files.get(name)
        if known is None:
            resolved = str(Path(name).resolve())
            known = any(resolved.startswith(root + os.sep) for root in self.roots)
            self.project_files[name] = known
        return known

    # -- facts -----------------------------------------------------------

    def key(self, cursor) -> str:
        template = getattr(cursor, "specialized_template", None)
        if template is not None and template.get_usr():
            return template.get_usr()
        return cursor.get_usr() or cursor.spelling

    def function(self, cursor) -> Function:
        key = self.key(cursor)
        entry = self.functions.get(key)
        if entry is None:
            entry = self.functions[key] = Function(cursor.spelling)
        return entry

    def special(self, cursor) -> bool:
        """A constructor, destructor or assignment operator: without a body in
        the FILEs it is taken as the implicit one, which changes nothing but
        its own object (an assignment is judged like a standard mutator)."""
        K = self.K
        return cursor.kind in (K.CONSTRUCTOR, K.DESTRUCTOR) or cursor.spelling == "operator="

    def declared(self, cursor) -> None:
        """A project function: whether it could mutate its object or its
        arguments, which decides when no FILE defines it."""
        K, T = self.K, self.T
        entry = self.function(cursor)
        if entry.defined:
            return
        try:
            if cursor.kind == K.CXX_METHOD and cursor.is_virtual_method():
                entry.virtual = True
        except AttributeError:
            pass
        if self.special(cursor):
            return
        parent = cursor.semantic_parent
        if cursor.kind in (K.CXX_METHOD, K.FUNCTION_TEMPLATE) and parent is not None and \
                parent.kind in self.scope_kinds and parent.kind != K.NAMESPACE and \
                not cursor.is_const_method() and not cursor.is_static_method():
            entry.conservative = True
        for argument in cursor.get_arguments() or []:
            parameter = argument.type
            if parameter.kind == T.LVALUEREFERENCE and \
                    not parameter.get_pointee().is_const_qualified():
                entry.conservative = True

    def root_outlives(self, node, depth: int = 0) -> bool:
        """Whether the object an lvalue expression designates outlives the
        enclosing call: a member, a global or static, or anything reached
        through a reference or a pointer."""
        K, T = self.K, self.T
        if depth > 40:
            return True
        kind = node.kind
        children = list(node.get_children())
        if kind in self.cast_kinds:
            return self.root_outlives(children[-1], depth + 1) if children else False
        if kind == K.MEMBER_REF_EXPR:
            if not children:
                return True  # implicit this
            base = children[0]
            if base.type.kind == T.POINTER:
                return True
            return self.root_outlives(base, depth + 1)
        if kind == K.DECL_REF_EXPR:
            referenced = node.referenced
            if referenced is None:
                return True
            if referenced.kind == K.VAR_DECL:
                if referenced.type.kind in (T.LVALUEREFERENCE, T.RVALUEREFERENCE):
                    return True  # may alias anything
                parent = referenced.semantic_parent
                if parent is None or parent.kind not in self.function_kinds:
                    return True  # namespace scope or static member
                return referenced.storage_class == self.cindex.StorageClass.STATIC
            if referenced.kind == K.PARM_DECL:
                return referenced.type.kind in (T.LVALUEREFERENCE, T.RVALUEREFERENCE)
            return referenced.kind == K.FIELD_DECL
        if kind == K.CXX_THIS_EXPR:
            return True
        if kind == K.ARRAY_SUBSCRIPT_EXPR:
            return self.root_outlives(children[0], depth + 1) if children else True
        if kind == K.UNARY_OPERATOR:
            return True  # *p
        if kind == K.CALL_EXPR:
            if node.type.kind not in (T.LVALUEREFERENCE, T.RVALUEREFERENCE, T.POINTER):
                return False  # a temporary
            target = self.object_of(node)
            return True if target is None else self.root_outlives(target, depth + 1)
        return False

    def object_of(self, call):
        """The object expression of a member or member-operator call."""
        K = self.K
        referenced = call.referenced
        if referenced is None or referenced.kind != K.CXX_METHOD:
            return None
        children = list(call.get_children())
        if not children:
            return None
        callee = children[0]
        while callee.kind == K.UNEXPOSED_EXPR:
            inner = list(callee.get_children())
            if not inner:
                break
            callee = inner[0]
        if callee.kind == K.MEMBER_REF_EXPR:
            members = list(callee.get_children())
            return members[0] if members else None
        if call.spelling.startswith("operator"):
            # An operator call lists the object as its first operand.
            arguments = list(call.get_arguments())
            if arguments:
                return arguments[0]
        return None

    def std_mutation(self, call) -> bool:
        """A call into code outside the project (the standard library) that
        changes an object outliving the enclosing call."""
        K, T = self.K, self.T
        referenced = call.referenced
        if referenced is None:
            return False
        name = referenced.spelling
        if referenced.kind == K.CXX_METHOD:
            if referenced.is_static_method() or referenced.is_const_method():
                return False
            parent = referenced.semantic_parent
            if name not in STD_MUTATORS and not (
                    name == "operator[]" and parent is not None and
                    MAP_TYPES.search(parent.spelling or "")):
                return False
            target = self.object_of(call)
            return True if target is None else self.root_outlives(target)
        if referenced.kind in (K.FUNCTION_DECL, K.FUNCTION_TEMPLATE):
            parameters = list(referenced.get_arguments() or [])
            for index, argument in enumerate(call.get_arguments()):
                if index >= len(parameters):
                    break
                parameter = parameters[index].type
                if parameter.kind == T.LVALUEREFERENCE and \
                        not parameter.get_pointee().is_const_qualified() and \
                        self.root_outlives(argument):
                    return True
        return False

    def steps(self, node) -> bool:
        """A unary ++ or -- (prefix or postfix)."""
        children = list(node.get_children())
        if not children:
            return False
        tokens = [token.spelling for token in node.get_tokens()]
        if not tokens:
            return False
        inner = children[0].extent
        if inner.start.offset > node.extent.start.offset and tokens[0] in ("++", "--"):
            return True
        return inner.end.offset < node.extent.end.offset and tokens[-1] in ("++", "--")

    def operator_spelling(self, node) -> str | None:
        K = self.K
        if node.kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR):
            children = list(node.get_children())
            if len(children) == 2:
                left_end = children[0].extent.end.offset
                right_start = children[1].extent.start.offset
                for token in node.get_tokens():
                    if left_end <= token.extent.start.offset < right_start:
                        return token.spelling
        if node.kind == K.CALL_EXPR and node.spelling.startswith("operator") and \
                len(node.spelling) > len("operator") and \
                not (node.spelling[len("operator")].isalnum() or node.spelling[len("operator")] == "_"):
            return node.spelling[len("operator"):].strip()
        return None

    def facts(self, cursor, into_lambdas: bool) -> Facts:
        """The facts of an expression or a body; lambda bodies count only with
        into_lambdas (a lambda passed as an argument does not run there)."""
        K = self.K
        result = Facts()
        stack = [cursor]
        while stack:
            node = stack.pop()
            kind = node.kind
            if kind == K.LAMBDA_EXPR and not into_lambdas:
                continue
            if kind == K.CALL_EXPR:
                referenced = node.referenced
                if referenced is None:
                    if node.spelling:
                        result.names.add(node.spelling)
                elif referenced.kind in self.function_kinds and self.in_project(referenced):
                    result.usrs.add(self.key(referenced))
                    self.declared(referenced)
                    if referenced.spelling == "operator=":
                        target = self.object_of(node)
                        if target is None or self.root_outlives(target):
                            result.mutates = True
                elif not self.in_project(referenced) and self.std_mutation(node):
                    result.mutates = True
            elif kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
                referenced = node.referenced
                if referenced is not None:
                    if referenced.kind in self.function_kinds and self.in_project(referenced):
                        result.usrs.add(self.key(referenced))
                        self.declared(referenced)
                    elif referenced.kind == K.VAR_DECL and \
                            referenced.get_usr() in self.lambda_variables:
                        result.usrs.add(referenced.get_usr())
                elif kind == K.MEMBER_REF_EXPR and node.spelling:
                    result.names.add(node.spelling)  # a dependent member
            elif kind == K.OVERLOADED_DECL_REF:
                if node.spelling:
                    result.names.add(node.spelling)
            elif kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR):
                children = list(node.get_children())
                if children and self.operator_spelling(node) in ASSIGNMENTS and \
                        self.root_outlives(children[0]):
                    result.mutates = True
            elif kind == K.UNARY_OPERATOR:
                children = list(node.get_children())
                if children and self.steps(node) and self.root_outlives(children[0]):
                    result.mutates = True
            elif kind == K.VAR_DECL:
                self.note_lambda_variable(node)
            stack.extend(node.get_children())
        return result

    def note_lambda_variable(self, variable) -> None:
        """A variable holding a lambda is a function of its own: calling it
        (or passing it on) has the lambda body's effects."""
        K = self.K
        usr = variable.get_usr()
        if not usr or usr in self.lambda_variables:
            return
        for child in variable.get_children():
            node = child
            while node.kind in self.cast_kinds and list(node.get_children()):
                node = list(node.get_children())[0]
            if node.kind == K.LAMBDA_EXPR:
                self.lambda_variables.add(usr)
                entry = self.functions.setdefault(usr, Function(variable.spelling))
                entry.defined = True
                entry.facts.add(self.facts(node, into_lambdas=True))
                return

    # -- collection ------------------------------------------------------

    def collect(self, cursor) -> None:
        """Every function defined in a project file of this unit."""
        K = self.K
        for child in cursor.get_children():
            if not self.in_project(child):
                continue
            if child.kind in self.scope_kinds:
                self.collect(child)
                continue
            if child.kind in self.function_kinds and child.is_definition():
                entry = self.function(child)
                if not entry.defined:
                    entry.defined = True
                    entry.conservative = False
                    entry.facts.add(self.facts(child, into_lambdas=True))
                try:
                    if child.kind == K.CXX_METHOD and child.is_virtual_method():
                        entry.virtual = True
                except AttributeError:
                    pass
            elif child.kind == K.VAR_DECL:
                self.note_lambda_variable(child)

    def braced(self, call, arguments) -> bool:
        """`T{a, b}`: list-initialization, sequenced left to right."""
        first = arguments[0].extent.start.offset
        opener = None
        for token in call.get_tokens():
            if token.extent.start.offset >= first:
                break
            if token.spelling in ("(", "{"):
                opener = token.spelling
        return opener == "{"

    def sites_of(self, cursor, path: Path) -> None:
        K = self.K
        main = str(path)
        for node in cursor.walk_preorder():
            location = node.location
            if location.file is None or location.file.name != main and \
                    str(Path(location.file.name).resolve()) != main:
                continue
            operator = self.operator_spelling(node)
            if operator == "()":
                # A call through an object: the object is sequenced before
                # the arguments, which are not sequenced among themselves.
                arguments = list(node.get_arguments())[1:]
                self.calls_checked += 1
                if len(arguments) >= 2:
                    self.add_site(node, path, "call through operator()", arguments, "arguments")
                continue
            if operator is not None:
                if operator in SEQUENCED:
                    continue
                operands = (list(node.get_arguments()) if node.kind == K.CALL_EXPR
                            else list(node.get_children()))
                self.add_site(node, path, f"operator{operator}", operands, "operands")
                continue
            if node.kind == K.CALL_EXPR:
                arguments = list(node.get_arguments())
                self.calls_checked += 1
                if len(arguments) < 2:
                    continue
                referenced = node.referenced
                if referenced is not None and referenced.kind == K.CONSTRUCTOR and \
                        self.braced(node, arguments):
                    continue
                self.add_site(node, path, f"call to {node.spelling or '?'}", arguments, "arguments")

    def add_site(self, node, path: Path, what: str, operands, noun: str) -> None:
        facts = [self.facts(operand, into_lambdas=False) for operand in operands]
        if sum(1 for f in facts if f.usrs or f.names or f.mutates) < 2:
            return  # two effectful operands are impossible here
        try:
            shown = path.relative_to(REPO)
        except ValueError:
            shown = path
        location = node.location
        self.sites.append(Site(f"{shown}:{location.line}:{location.column}",
                               f"{what} with {{count}} effectful {noun}", facts))

    # -- the effectful set -----------------------------------------------

    def effectful(self) -> set[str]:
        result = {usr for usr, entry in self.functions.items()
                  if entry.facts.mutates or (not entry.defined and entry.conservative)}
        names = {self.functions[usr].name for usr in result}
        changed = True
        while changed:
            changed = False
            for usr, entry in self.functions.items():
                if usr in result:
                    continue
                if entry.facts.usrs & result or entry.facts.names & names or \
                        (entry.virtual and entry.name in names):
                    result.add(usr)
                    names.add(entry.name)
                    changed = True
        return result

    def findings(self) -> tuple[set[str], list[str]]:
        effectful = self.effectful()
        names = {self.functions[usr].name for usr in effectful}
        reports = []
        for site in self.sites:
            count = sum(1 for f in site.operands
                        if f.mutates or f.usrs & effectful or f.names & names)
            if count >= 2:
                reports.append(f"{site.location}: {site.message.format(count=count)}")
        return effectful, reports


def base_flags(library: str | None) -> list[str]:
    flags = ["-x", "c++", "-std=c++20", "-DNDEBUG"]
    resources = resource_dir(library)
    if resources:
        flags += ["-resource-dir", resources]
    sdk = os.popen("xcrun --show-sdk-path 2>/dev/null").read().strip()
    if sdk:
        flags += ["-isysroot", sdk]
    return flags


def run(cindex, library, files: list[Path], include: list[str],
        roots: list[Path]) -> tuple[Lint, set[str], list[str]]:
    index = cindex.Index.create()
    flags = base_flags(library) + [f"-I{path}" for path in include]
    lint = Lint(cindex, roots)
    for path in files:
        unit = index.parse(str(path), args=flags)
        errors = [d for d in unit.diagnostics if d.severity >= cindex.Diagnostic.Error]
        if errors:
            for diagnostic in errors[:5]:
                print(f"check_sequencing.py: {diagnostic}", file=sys.stderr)
            raise SystemExit(2)
        lint.collect(unit.cursor)
        lint.sites_of(unit.cursor, path)
        del unit
    effectful, findings = lint.findings()
    return lint, effectful, findings


SELF_TEST_PROBE = r'''
#include <sstream>
#include <string>
#include <vector>

struct TemporaryNames {
    int counter = 0;
    std::string temp() { return "%t" + std::to_string(counter++); }
};
struct FunctionText {
    std::ostringstream out;
    void append(const std::string& text) { out << text; }
};
struct TextIdioms {  // effectful only through its callee
    TemporaryNames& names;
    std::string match(int) { return names.temp(); }
};
struct SelfDepthGuard {
    TemporaryNames& names;
    std::string wrapper() { return names.temp(); }
};
struct FunctionContext {
    std::vector<std::string> lines;
};
struct Lookup {  // non-const accessors that change nothing
    std::vector<int> values;
    int at(std::size_t i) { return values.at(i); }
};
std::string take(FunctionContext& context) {
    context.lines.push_back("x");
    return context.lines.back();
}
std::string join(const std::string& a, const std::string& b) { return a + b; }
int append_once(FunctionText& text) { text.append("a"); return 0; }
std::string remote(TemporaryNames& names);  // defined in no file of the run

void probe(TemporaryNames& names, TextIdioms& idioms, SelfDepthGuard& guard,
           FunctionContext& context, FunctionText& text, Lookup& lookup) {
    join(names.temp(), names.temp());                                   // FINDING
    join(idioms.match(1), idioms.match(2));                             // FINDING
    join(guard.wrapper(), guard.wrapper());                             // FINDING
    join(take(context), take(context));                                 // FINDING
    (void)(append_once(text) + append_once(text));                      // FINDING
    join(remote(names), remote(names));                                 // FINDING
    const auto next = [&] { return names.temp(); };
    join(next(), next());                                               // FINDING
    const auto pair = [](const std::string& a, const std::string& b) { return a + b; };
    pair(names.temp(), names.temp());                                   // FINDING
    pair(names.temp(), std::string("x"));
    Lookup copy = lookup;
    join(std::to_string(copy.at(0)), names.temp());
    join(std::to_string(lookup.at(0)), std::to_string(lookup.at(1)));
    std::string braced[] = {names.temp(), names.temp()};
    std::ostringstream out;
    out << names.temp() << names.temp();
    const auto first = names.temp();
    join(first, names.temp());
    (void)braced;
}
'''


def self_test(cindex, library) -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        path = root / "probe.cpp"
        path.write_text(SELF_TEST_PROBE, encoding="utf-8")
        _, _, findings = run(cindex, library, [path], [], [root])
    expected = sorted(index + 1 for index, line in enumerate(SELF_TEST_PROBE.splitlines())
                      if line.rstrip().endswith("// FINDING"))
    found = sorted(int(finding.rsplit(":", 3)[1]) for finding in findings)
    ok = found == expected
    print(f"check_sequencing.py: self-test: findings on lines {found}, expected {expected}: "
          f"{'ok' if ok else 'FAILED'}")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=str(REPO / "build-release"))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    cindex, library = import_clang()
    if args.self_test:
        return self_test(cindex, library)
    files = [(REPO / name).resolve() for name in (args.files or default_files())]
    include = [str(REPO / "include"), str(Path(args.build) / "generated"), str(REPO / "src")]
    lint, effectful, findings = run(cindex, library, files, include, [REPO])
    defined = sum(1 for usr in effectful if lint.functions[usr].defined)
    print(f"check_sequencing.py: {len(files)} files, {len(lint.functions)} project functions, "
          f"{len(effectful)} effectful ({defined} defined in the files), "
          f"{lint.calls_checked} calls, {len(lint.sites)} sites with two candidate operands, "
          f"{len(findings)} findings")
    status = 1 if findings else 0
    for name in NAME_ALLOCATORS:
        present = [usr for usr, f in lint.functions.items() if f.name == name and f.defined]
        if present and not any(usr in effectful for usr in present):
            print(f"check_sequencing.py: name allocator {name} is not derived as effectful")
            status = 1
    for finding in findings:
        print("  " + finding)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
