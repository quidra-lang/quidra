#pragma once

// BuiltinLowering: a builtin call, dispatched to the unit of its domain.
// Defined in builtin_lowering.cpp.

#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class OperatorLowering;
class AggregateLowering;
class TensorLowering;
class AutogradLowering;
class ConsoleLowering;
class FileLowering;
class SystemLowering;
class JsonLowering;
class HttpLowering;
class ConcurrencyLowering;
class RandomLowering;
class CheckLowering;
class ReflectionLowering;
class ScanLowering;
class ControlFlowLowering;

class BuiltinLowering {
public:
    BuiltinLowering(
        OperatorLowering& operators, AggregateLowering& aggregates,
        TensorLowering& tensors, AutogradLowering& autograd,
        ConsoleLowering& console, FileLowering& files, SystemLowering& system,
        JsonLowering& json, HttpLowering& http,
        ConcurrencyLowering& concurrency, RandomLowering& random,
        CheckLowering& checks, ReflectionLowering& reflection,
        ScanLowering& scan, ControlFlowLowering& control_flow)
        : operators_(operators), aggregates_(aggregates), tensors_(tensors),
          autograd_(autograd), console_(console), files_(files),
          system_(system), json_(json), http_(http), concurrency_(concurrency),
          random_(random), checks_(checks), reflection_(reflection),
          scan_(scan), control_flow_(control_flow) {}

    // a builtin call, dispatched to its domain
    ValueId lower_builtin_call(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);

private:
    OperatorLowering& operators_;
    AggregateLowering& aggregates_;
    TensorLowering& tensors_;
    AutogradLowering& autograd_;
    ConsoleLowering& console_;
    FileLowering& files_;
    SystemLowering& system_;
    JsonLowering& json_;
    HttpLowering& http_;
    ConcurrencyLowering& concurrency_;
    RandomLowering& random_;
    CheckLowering& checks_;
    ReflectionLowering& reflection_;
    ScanLowering& scan_;
    ControlFlowLowering& control_flow_;
};

} // namespace quidra::lowering
