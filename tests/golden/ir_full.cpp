// Field-complete serialization of the typed IR; see ir_full.hpp.
#include "ir_full.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quidra::golden {
namespace {

// Every overload is declared before any template body uses it, because the
// standard containers' associated namespaces never include this one.
void put(std::string& out, bool value);
void put(std::string& out, double value);
void put(std::string& out, const std::string& value);
void put(std::string& out, const Type& type);
void put(std::string& out, const std::shared_ptr<Type>& type);
void put(std::string& out, const ir::Binary& node);
void put(std::string& out, const CompilerExtensionRegistration& extension);
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
void put(std::string& out, T value);
template <class T>
    requires std::is_enum_v<T>
void put(std::string& out, T value);
template <class T>
void put(std::string& out, const std::optional<T>& value);
template <class T>
void put(std::string& out, const std::vector<T>& values);
template <class K, class V>
void put(std::string& out, const std::map<K, V>& values);
template <class T>
    requires(std::is_class_v<T> && std::is_aggregate_v<T>)
void put(std::string& out, const T& value);

void put(std::string& out, bool value) { out += value ? '1' : '0'; }

void put(std::string& out, double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "f64:%016llx",
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(value)));
    out += buffer;
}

void put(std::string& out, const std::string& value) { out += quote(value); }

template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
void put(std::string& out, T value) {
    if constexpr (std::is_signed_v<T>) {
        out += std::to_string(static_cast<long long>(value));
    } else {
        out += std::to_string(static_cast<unsigned long long>(value));
    }
}

template <class T>
    requires std::is_enum_v<T>
void put(std::string& out, T value) {
    put(out, static_cast<std::underlying_type_t<T>>(value));
}

template <class T>
void put(std::string& out, const std::optional<T>& value) {
    if (!value) {
        out += "none";
        return;
    }
    put(out, *value);
}

template <class T>
void put(std::string& out, const std::vector<T>& values) {
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        put(out, values[i]);
    }
    out += ']';
}

template <class K, class V>
void put(std::string& out, const std::map<K, V>& values) {
    out += '{';
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) out += ", ";
        first = false;
        put(out, key);
        out += ": ";
        put(out, value);
    }
    out += '}';
}

void put(std::string& out, const Type& type) {
    // Type gains a field -> this binding stops compiling.
    const auto& [kind, first, cases, parameters, length, shape_prefix, known_shape_prefix,
                 class_name, union_name, case_names] = type;
    out += "T{";
    put(out, kind);
    out += ", ";
    put(out, first);
    out += ", ";
    put(out, cases);
    out += ", ";
    put(out, parameters);
    out += ", ";
    put(out, length);
    out += ", ";
    put(out, shape_prefix);
    out += ", ";
    put(out, known_shape_prefix);
    out += ", ";
    put(out, class_name);
    out += ", ";
    put(out, union_name);
    out += ", ";
    put(out, case_names);
    out += '}';
}

void put(std::string& out, const std::shared_ptr<Type>& type) {
    if (!type) {
        out += "null";
        return;
    }
    put(out, *type);
}

// Binary has a constructor, so it is not an aggregate.
void put(std::string& out, const ir::Binary& node) {
    out += '{';
    put(out, node.out);
    out += ", ";
    put(out, node.op);
    out += ", ";
    put(out, node.left);
    out += ", ";
    put(out, node.right);
    out += ", ";
    put(out, node.operand_type);
    out += ", ";
    put(out, node.result_type);
    out += ", ";
    put(out, node.line);
    out += ", ";
    put(out, node.column);
    out += ", ";
    put(out, node.overflow_proven);
    out += ", ";
    put(out, node.inline_proven);
    out += '}';
}

void put(std::string& out, const CompilerExtensionRegistration& extension) {
    const auto& [package, name, package_root, descriptor_path, descriptor, phase, tables] =
        extension;
    out += '{';
    put(out, package);
    out += ", ";
    put(out, name);
    out += ", ";
    put(out, package_root);
    out += ", ";
    put(out, descriptor_path);
    out += ", ";
    put(out, descriptor);
    out += ", ";
    put(out, phase);
    out += ", ";
    put(out, tables);
    out += '}';
}

template <class... F>
void put_fields(std::string& out, const F&... fields) {
    out += '{';
    std::size_t index = 0;
    ((out += (index++ ? ", " : ""), put(out, fields)), ...);
    out += '}';
}

// ClassLayout gains a field -> this binding stops compiling. The
// standard_library flag is not printed: it is the class's origin, not an IR
// value, and what it decides in the backend (inlined collection methods,
// ref.Cell clones) shows in the LLVM text.
void put(std::string& out, const ir::ClassLayout& layout) {
    const auto& [name, field_names, fields, standard_library] = layout;
    (void)standard_library;
    put_fields(out, name, field_names, fields);
}

template <class T>
    requires(std::is_class_v<T> && std::is_aggregate_v<T>)
void put(std::string& out, const T& value) {
    constexpr std::size_t n = aggregate_arity<T>();
    static_assert(n <= max_aggregate_arity, "extend the binding ladder in ir_full.cpp");
    if constexpr (n == 0) {
        out += "{}";
    } else if constexpr (n == 1) {
        const auto& [a] = value;
        put_fields(out, a);
    } else if constexpr (n == 2) {
        const auto& [a, b] = value;
        put_fields(out, a, b);
    } else if constexpr (n == 3) {
        const auto& [a, b, c] = value;
        put_fields(out, a, b, c);
    } else if constexpr (n == 4) {
        const auto& [a, b, c, d] = value;
        put_fields(out, a, b, c, d);
    } else if constexpr (n == 5) {
        const auto& [a, b, c, d, e] = value;
        put_fields(out, a, b, c, d, e);
    } else if constexpr (n == 6) {
        const auto& [a, b, c, d, e, f] = value;
        put_fields(out, a, b, c, d, e, f);
    } else if constexpr (n == 7) {
        const auto& [a, b, c, d, e, f, g] = value;
        put_fields(out, a, b, c, d, e, f, g);
    } else if constexpr (n == 8) {
        const auto& [a, b, c, d, e, f, g, h] = value;
        put_fields(out, a, b, c, d, e, f, g, h);
    } else if constexpr (n == 9) {
        const auto& [a, b, c, d, e, f, g, h, i] = value;
        put_fields(out, a, b, c, d, e, f, g, h, i);
    } else if constexpr (n == 10) {
        const auto& [a, b, c, d, e, f, g, h, i, j] = value;
        put_fields(out, a, b, c, d, e, f, g, h, i, j);
    } else if constexpr (n == 11) {
        const auto& [a, b, c, d, e, f, g, h, i, j, k] = value;
        put_fields(out, a, b, c, d, e, f, g, h, i, j, k);
    } else {
        const auto& [a, b, c, d, e, f, g, h, i, j, k, l] = value;
        put_fields(out, a, b, c, d, e, f, g, h, i, j, k, l);
    }
}

// An instruction is named, not numbered: adding or removing an alternative
// leaves every other instruction's rendering unchanged.
void put_instruction(std::string& out, const ir::Instruction& instruction) {
    out += instruction_names[instruction.index()];
    std::visit([&](const auto& node) { put(out, node); }, instruction);
}

} // namespace

std::string quote(std::string_view text) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '"' || byte == '\\') {
            out += '\\';
            out += raw;
        } else if (byte >= 0x20 && byte < 0x7f) {
            out += raw;
        } else {
            out += "\\x";
            out += digits[byte >> 4];
            out += digits[byte & 0x0f];
        }
    }
    out += '"';
    return out;
}

std::string serialize_type(const Type& type) {
    std::string out;
    put(out, type);
    return out;
}

std::string serialize_instruction(const ir::Instruction& instruction) {
    std::string out;
    put_instruction(out, instruction);
    return out;
}

std::string serialize_module(const ir::Module& module) {
    // Module, Function and Block gain a field -> these bindings stop compiling.
    const auto& [classes, functions, compiler_extensions, user_sources] = module;
    std::string out = "module\n";
    for (std::size_t i = 0; i < user_sources.size(); ++i) {
        out += "user_source #" + std::to_string(i) + ' ';
        put(out, user_sources[i]);
        out += '\n';
    }
    for (std::size_t i = 0; i < classes.size(); ++i) {
        out += "class #" + std::to_string(i) + ' ';
        put(out, classes[i]);
        out += '\n';
    }
    for (std::size_t i = 0; i < compiler_extensions.size(); ++i) {
        out += "extension #" + std::to_string(i) + ' ';
        put(out, compiler_extensions[i]);
        out += '\n';
    }
    for (std::size_t f = 0; f < functions.size(); ++f) {
        const auto& [name, source_file, source_line, source_column, parameters, result,
                     blocks, tensor_regions, entrypoint, external_symbol,
                     c_export_symbol] = functions[f];
        out += "function #" + std::to_string(f) + ' ';
        put(out, name);
        out += "\n  source ";
        put(out, source_file);
        out += ' ';
        put(out, source_line);
        out += ':';
        put(out, source_column);
        out += "\n  parameters ";
        put(out, parameters);
        out += "\n  result ";
        put(out, result);
        out += "\n  entrypoint ";
        put(out, entrypoint);
        out += "\n  external_symbol ";
        put(out, external_symbol);
        // Only exported functions have the line, so that the view of a
        // program without exports is unchanged.
        if (c_export_symbol) {
            out += "\n  c_export_symbol ";
            put(out, c_export_symbol);
        }
        out += '\n';
        for (std::size_t r = 0; r < tensor_regions.size(); ++r) {
            out += "  region #" + std::to_string(r) + ' ';
            put(out, tensor_regions[r]);
            out += '\n';
        }
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto& [label, instructions] = blocks[b];
            out += "  block #" + std::to_string(b) + ' ';
            put(out, label);
            out += '\n';
            for (std::size_t i = 0; i < instructions.size(); ++i) {
                out += "    " + std::to_string(i) + ' ';
                put_instruction(out, instructions[i]);
                out += '\n';
            }
        }
        out += "end\n";
    }
    return out;
}

} // namespace quidra::golden
