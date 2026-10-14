#include "quidra/compiler_extension.hpp"
#include "quidra/compiler_extension_model.hpp"
#include "quidra/compile_inputs.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace fs = std::filesystem;
using namespace quidra;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static CompilerExtensionRegistration parse(const fs::path& root, std::string text,
                                           CompileInputs* inputs = nullptr) {
    return parse_compiler_extension_descriptor("generic", "graph", root,
        root / "graph.toml", std::move(text), inputs);
}
static void rejected(const fs::path& root, std::string text, std::string_view expected) {
    try { (void)parse(root, std::move(text)); }
    catch (const std::exception& e) {
        require(std::string_view(e.what()).find(expected) != std::string_view::npos,
                "wrong descriptor diagnostic");
        return;
    }
    throw std::runtime_error("invalid descriptor accepted");
}
static const std::string header = "[extension]\nversion = 2\nphase = \"tensor-program\"\n";
static const std::string operation =
    "[operation.op]\nfunction = \"work<T: floating>(tensor<T>,int64)|other\"\n"
    "traits = \"tensor,pure,elementwise,finite-preserving,collective\"\n"
    "effects = \"reads-policy\"\ndata = \"value\"\nreads = \"value\"\n"
    "writes = \"result:full\"\ninplace = \"value->result\"\ncapture = \"safe\"\n"
    "validates = \"nonfinite\"\npreconditions = \"value.0 > 0 and policy == 0\"\n";
int main(int argc, char** argv) {
    try {
        require(argc == 2, "temporary directory argument required");
        const fs::path root = argv[1];
        fs::create_directories(root / "kernels");
        { std::ofstream file(root / "kernels" / "work.qk"); file << "generic kernel snapshot\n"; }
        const auto minimal = parse(root, header + operation);
        require(minimal.schema_revision == 2 && minimal.model,
                "full descriptor model is missing");
        require(minimal.phase == "tensor-program" && minimal.files.empty(), "full identity changed");
        const auto& fields = minimal.model->tables.at("operation.op").fields;
        require(std::get<std::vector<CompilerDeclarationSelector>>(fields.at("function")).size() == 2,
                "selector alternatives lost");
        require(std::get<CompilerExpression>(fields.at("preconditions")).category ==
                CompilerExpression::Category::Predicate, "predicate was not typed");
        const auto legacy = parse(root,
            "[extension]\nversion = 2\nphase = \"tensor-region\"\n"
            "[operation.old]\nfunction = \"work\"\nlegacy_private_key = \"accepted\"\n");
        require(legacy.schema_revision == 1 && !legacy.model && legacy.files.empty(), "legacy model changed");
        require(legacy.tables.at("operation.old").at("legacy_private_key") == "accepted", "legacy bytes changed");
        rejected(root, "[extension]\nversion = 1\nphase = \"tensor-region\"\n", "version = 2");
        rejected(root, header + "[operation.bad]\nfunction = \"x\"\ntraits = \"tensor,pure\"\nunknown = 1\n", "operation.bad.unknown");
        const std::string full = header + operation + R"toml(
[operation.replacement]
function = "replacement"
traits = "tensor,pure,collective"
[execution_policy.deterministic]
function = "set_policy"
[fusion.chain]
operations = "op,replacement"
replacement = "replacement"
priority = 10
[pattern.chain]
nodes = "a=op,b=@activation"
edges = "a->b"
outputs = "b"
select = "cost"
lower = "kernel:generated"
fallback = "replacement"
numerics = "opaque-group"
[specialization.shape]
operation = "op"
replacement = "replacement"
dtype = "real32"
rank = 2
shape = "2,3"
same_shape = "value,other"
policy = "deterministic"
[backend.device]
operation = "op"
replacement = "replacement"
backend = "metal"
layout = "strided"
tracked = "false"
[memory.reuse]
operation = "op"
replacement = "replacement"
last_use = true
owned = "true"
[donation.inplace]
operation = "op"
replacement = "replacement"
donate = "value"
[kernel.native]
operation = "op"
backend = "cpu"
role = "native"
admit = "generic_admit"
encode = "generic_encode"
tensors = "value,result"
params = "small:i32,wide:i64,ratio:f32"
alias_safe = true
workspace = "admit"
host_sync = "status-buffer"
numerics = "opaque-group"
offsets = "zero"
[kernel.generated]
operation = "op"
backend = "metal"
role = "generated"
file = "kernels/./work.qk"
entry = "work"
tensors = "value,result,pre"
outputs_extra = "pre:residual"
applies_when = "M > 0 and align(value) >= 4"
bind = "M=value.0"
numerics = "opaque-group"
cost = "op"
[kernel.template]
operation = "op"
backend = "test"
role = "template"
file = "kernels/work.qk"
entry = "work"
tensors = "value,result"
slots = "epilogue"
epilogue = "f32-store"
numerics = "opaque-group"
[fragment.local]
file = "kernels/work.qk"
entry = "work"
implements = "op"
dtype = "real32"
numerics = "opaque-group"
backends = "cpu,metal,test"
[intrinsic.local]
metal = "opaque_native_expression"
numerics = "opaque-group"
[cost.op]
bind = "M=value.0,N=wide"
flops = "2*M*N"
bytes_read = "4*(M+N)"
bytes_written = "ceil(M,4)*4"
launches = "1"
efficiency = "metal=700,cpu=300"
shared = "min(M,1024)"
threads = "max(32,M)"
[derivative.local]
operation = "op"
differentiable = "value"
saves = ""
metadata = "small:i32,wide:i64,ratio:f32"
vjp_native = "generic_vjp"
vjp_function = "replacement"
vjp_higher_order = "replacement"
primal_untracked_equivalent = true
recompute = "elementwise"
residual = "mask:opaque"
residual_vjp_native = "generic_residual_vjp"
mask = true
full_writes = "value"
[tuning.local]
kernel = "generated"
key = "dtype,value.shape,backend"
space = "threads=32|64|128"
numerics = "invariant"
policy = "any"
[collective.local]
operation = "op"
combine = "replacement"
combine_numerics_deterministic = "bitwise"
[query.local]
function = "generic_query"
depends = "backend,device,dtype,contiguous,rank"
[policy]
opaque = "documentation"
[analysis]
notes = "documentation"
[x-vendor.local]
custom = "opaque"
)toml";
        CompileInputs inputs;
        const auto model = parse(root, full, &inputs);
        require(model.model->tables.size() == 23, "full schema tables lost");
        require(model.files.size() == 1 && model.files.at("kernels/work.qk") == "generic kernel snapshot\n",
                "referenced files were not normalized/snapshotted");
        require(inputs.complete() && inputs.records().size() == 1 &&
                std::get<InputFile>(inputs.records().front()).kind == InputFileKind::descriptor,
                "source snapshot did not use compile-input recorder");
        const auto& metadata = std::get<std::vector<CompilerPodField>>(
            model.model->tables.at("derivative.local").fields.at("metadata"));
        require(metadata.size() == 3 && metadata[0].offset == 0 && metadata[1].offset == 8 &&
                metadata[2].offset == 16 && metadata[1].size == 8, "natural POD layout is incorrect");
        require(std::get<std::int64_t>(model.model->tables.at("tuning.local").fields.at("repeats")) == 5,
                "normalized defaults missing");
        require(std::get<std::string>(model.model->tables.at("kernel.generated").fields.at("offsets")) == "any",
                "merged offset default missing");
        require(model.model->tables.at("x-vendor.local").kind == CompilerExtensionTableKind::Passthrough,
                "opaque extension not retained");
        // The registration owns snapshots even when source content later changes.
        { std::ofstream file(root / "kernels" / "work.qk"); file << "changed\n"; }
        require(model.files.at("kernels/work.qk") == "generic kernel snapshot\n", "snapshot was reread");
        rejected(root, header + operation + "[query.bad]\nfunction = \"q\"\ndepends = \"unknown\"\n", "query.bad.depends");
        rejected(root, header + operation + "[donation.bad]\noperation = \"op\"\nreplacement = \"op\"\ndonate = \"other\"\n", "donation.bad.donate");
        rejected(root, header + operation + "[kernel.bad]\noperation = \"missing\"\n", "kernel.bad.operation");
        rejected(root, header + operation + "[cost.bad]\nbind = \"M=value.0\"\nflops = \"M > 0\"\nbytes_read = \"0\"\nbytes_written = \"0\"\nlaunches = \"1\"\n", "cost.bad.flops");
        rejected(root, header + operation + "[kernel.bad]\noperation = \"op\"\nbackend = \"cpu\"\nrole = \"generated\"\nfile = \"../bad.qk\"\n", "kernel.bad.file");
        rejected(root, header + "[operation.bad]\nfunction = \"work\"\ntraits = \"tensor,pure\" # trailing\n", "line 6");
        std::cout << "compiler extension tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
