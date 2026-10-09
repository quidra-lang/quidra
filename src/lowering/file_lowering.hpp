#pragma once

// FileLowering: file.Handle and the file builtins. Defined in
// file_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class FileLowering {
public:
    FileLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime) {}

    // file.Handle, the file builtins
    std::optional<ValueId> try_file_handle_method(
        const Expr& e, const MethodCallExpr& n, const Type& receiver_type);
    ValueId lower_file_open(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);
    ValueId lower_file_read(const Expr& e, const CallExpr& n);
    ValueId lower_file_read_bin(const Expr& e, const CallExpr& n);
    ValueId lower_file_list(const Expr& e, const CallExpr& n);
    ValueId lower_file_write(const Expr& e, const CallExpr& n);
    ValueId lower_file_write_bin(const Expr& e, const CallExpr& n);
    ValueId lower_file_exists(const Expr& e, const CallExpr& n);
    ValueId lower_file_is_directory(const Expr& e, const CallExpr& n);
    ValueId lower_file_remove(const Expr& e, const CallExpr& n);
    ValueId lower_file_copy(const Expr& e, const CallExpr& n);
    ValueId lower_file_move(const Expr& e, const CallExpr& n);
    ValueId lower_file_mkdir(const Expr& e, const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
