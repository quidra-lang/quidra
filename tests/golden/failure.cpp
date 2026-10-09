// Rendering of compilation failures; see failure.hpp.
#include "failure.hpp"

#include "ir_full.hpp"

#include "quidra/diagnostic.hpp"

#include <cstdlib>
#include <exception>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <typeinfo>

#if !defined(_MSC_VER)
#include <cxxabi.h>
#endif

namespace quidra::golden {
namespace {

// The readable name of a class, without the standard libraries' inline
// namespaces (libc++ `__1`, libstdc++ `__cxx11`), so that it reads the same
// on every platform.
std::string class_name(const std::type_info& type) {
#if defined(_MSC_VER)
    std::string name = type.name();  // "class std::out_of_range"
    for (const std::string_view prefix : {std::string_view("class "), std::string_view("struct ")}) {
        if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
    }
#else
    int status = 0;
    const std::unique_ptr<char, void (*)(void*)> demangled(
        ::abi::__cxa_demangle(type.name(), nullptr, nullptr, &status), std::free);
    std::string name = status == 0 && demangled ? demangled.get() : type.name();
#endif
    for (const std::string_view inline_namespace :
         {std::string_view("__1::"), std::string_view("__cxx11::")}) {
        for (auto at = name.find(inline_namespace); at != std::string::npos;
             at = name.find(inline_namespace)) {
            name.erase(at, inline_namespace.size());
        }
    }
    return name;
}

// ` type=CLASS` when the exception was thrown as a class derived from the
// handler's (std::out_of_range caught as std::logic_error), so that a change
// of exception class shows in the view even when the message stays.
std::string thrown_as(const std::exception& error, const std::type_info& handler) {
    if (typeid(error) == handler) return "";
    return " type=" + class_name(typeid(error));
}

std::string render_diagnostic(const Diagnostic& d) {
    std::ostringstream out;
    out << d.code << ' ' << d.span.start.line << ':' << d.span.start.column << '-'
        << d.span.end.line << ':' << d.span.end.column << " @" << d.span.start.offset << '-'
        << d.span.end.offset << ' ' << quote(d.message) << '\n';
    return out.str();
}

} // namespace

Failure current_failure() {
    try {
        throw;
    } catch (const CompileErrors& errors) {
        std::string text = "CompileErrors count=" + std::to_string(errors.diagnostics().size()) +
                           " truncated=" + (errors.truncated() ? "1" : "0") + "\n";
        for (const auto& d : errors.diagnostics()) text += render_diagnostic(d);
        const std::string code =
            errors.diagnostics().empty() ? "CompileErrors" : errors.diagnostics().front().code;
        return {"error:" + code, text};
    } catch (const CompileError& error) {
        return {"error:" + error.diagnostic().code,
                "CompileError\n" + render_diagnostic(error.diagnostic())};
    } catch (const std::logic_error& error) {
        return {"error:logic_error", "logic_error" + thrown_as(error, typeid(std::logic_error)) +
                                         " " + quote(error.what()) + "\n"};
    } catch (const std::exception& error) {
        return {"error:exception", "exception" + thrown_as(error, typeid(std::exception)) + " " +
                                       quote(error.what()) + "\n"};
    } catch (...) {
#if defined(_MSC_VER)
        return {"error:unknown", "unknown exception\n"};
#else
        const auto* type = ::abi::__cxa_current_exception_type();
        return {"error:unknown",
                "unknown exception" + (type ? " type=" + class_name(*type) : std::string()) + "\n"};
#endif
    }
}

} // namespace quidra::golden
