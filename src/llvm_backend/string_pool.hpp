#pragma once

// The module's pool of string constants.
//
// Owns: every string constant of a module (@.str.N, numbered in the order of
// first interning, one constant per distinct value) and the source
// provenance records of runtime failures (@.quidra.source.N, each pointing at
// interned strings). All functions of a module share one pool, so the order
// in which emission interns strings is part of the output.

#include "quidra/ir/module.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quidra::llvm_backend {

struct SourceProvenanceConstant {
    std::string name;
    std::string file;
    std::string revision;
    std::string node_id;
    std::string node_kind;
    unsigned long long line{};
    unsigned long long column{};
};

struct StringPool {
    std::vector<std::pair<std::string,std::string>> entries;
    std::unordered_map<std::string,std::string> names;
    std::vector<SourceProvenanceConstant> provenance_entries;
    // The caches of big integer literals (".int.literal.N"), in the order of
    // first interning, one per distinct spelling.
    std::vector<std::string> integer_literal_caches;
    std::unordered_map<std::string,std::string> integer_literal_names;

    // The name of the constant holding value (".str.N"), created on first use.
    // Defined in the class: every string constant of the module passes
    // through it.
    std::string intern(const std::string& value) {
        if (auto it = names.find(value); it != names.end()) return it->second;
        auto name = ".str." + std::to_string(entries.size());
        entries.emplace_back(name, value);
        names.emplace(value, name);
        return name;
    }
    // The name of the cache of the big integer literal `spelling`
    // (".int.literal.N"), created on first use; the module defines it as an
    // i64 global holding 0 until the literal's word is made.
    std::string intern_integer_literal(const std::string& spelling) {
        if (auto it = integer_literal_names.find(spelling); it != integer_literal_names.end())
            return it->second;
        auto name = ".int.literal." + std::to_string(integer_literal_caches.size());
        integer_literal_caches.push_back(name);
        integer_literal_names.emplace(spelling, name);
        return name;
    }
    // A new provenance record for location (".quidra.source.N"); its strings
    // are interned in the order file, revision, node id, node kind.
    std::string intern_provenance(const ir::SourceLocation& location);
};

} // namespace quidra::llvm_backend
