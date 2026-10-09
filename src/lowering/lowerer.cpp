// The Lowerer's construction. Its state first: the SourceLocator inspects
// every source text once, then BorrowInference decides the borrowed
// parameters of the whole program, before any function is lowered (member
// order). Then the units, each given references to the parts of the state
// it uses and to the units it calls; they bind references only, so their
// order does not matter.

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

Lowerer::Lowerer(
    const CheckedProgram& c, const Expr* repl,
    std::size_t replay_prefix_offset, const ir::LoweringOptions& options)
    : checked_(c), options_(options), facts_(c, scope_),
      replay_(repl, replay_prefix_offset), locator_(c), borrows_(c),
      storage_writes_(checked_, borrows_, builder_, scope_),
      assignment_order_(checked_, borrows_),
      isolation_(checked_, options.copy_every_borrowed_argument),
      initialization_flags_(checked_, builder_, scope_),
      lifetime_(checked_, builder_),
      conversions_(*this, checked_, builder_, scope_, lifetime_),
      shape_constraints_(*this, checked_, builder_, scope_),
      expressions_(checked_, builder_, scope_, facts_, nesting_depth_,
          enclosing_class_, initialization_flags_, lifetime_, conversions_,
          operators_, calls_, autograd_),
      operators_(*this, checked_, builder_, scope_, facts_, lifetime_,
          conversions_),
      calls_(*this, checked_, builder_, scope_, enclosing_class_, borrows_,
          isolation_, lifetime_, conversions_, builtins_, text_, bin_, aggregates_,
          tensors_, autograd_, files_, concurrency_),
      builtins_(operators_, aggregates_, tensors_, autograd_, console_, files_,
          system_, json_, http_, concurrency_, random_, checks_, reflection_,
          scan_, control_flow_),
      text_(*this, checked_, builder_, lifetime_),
      bin_(*this, builder_),
      aggregates_(*this, checked_, builder_, scope_, lifetime_, conversions_),
      tensors_(*this, checked_, builder_, shapes_, lifetime_, shape_constraints_,
          autograd_),
      autograd_(*this, checked_, builder_, lifetime_, reflection_),
      console_(*this, checked_, builder_, lifetime_),
      files_(*this, checked_, builder_, lifetime_),
      system_(*this, checked_, builder_, lifetime_),
      json_(*this, checked_, builder_, enclosing_class_, lifetime_),
      http_(*this, checked_, builder_, enclosing_class_, lifetime_),
      concurrency_(*this, checked_, builder_, lifetime_),
      random_(*this, builder_, enclosing_class_),
      checks_(*this, checked_, builder_, lifetime_),
      reflection_(*this, checked_, builder_, scope_, lifetime_),
      scan_(*this, checked_, builder_, scope_, lifetime_),
      statements_(*this, checked_, builder_, scope_, facts_, nesting_depth_,
          loop_targets_, enclosing_class_, replay_, shapes_, locator_, lifetime_,
          conversions_, shape_constraints_, calls_, assignments_,
          control_flow_, text_idioms_, functions_),
      assignments_(*this, checked_, builder_, scope_, facts_, enclosing_class_,
          shapes_, storage_writes_, assignment_order_, lifetime_,
          shape_constraints_),
      control_flow_(*this, checked_, builder_, scope_, facts_, loop_targets_,
          borrows_, storage_writes_, lifetime_, conversions_, text_idioms_,
          collection_idioms_),
      text_idioms_(*this, checked_, builder_, scope_, facts_, loop_targets_,
          locator_, borrows_, lifetime_),
      collection_idioms_(*this, checked_, builder_, scope_, facts_),
      functions_(*this, checked_, module_, builder_, scope_, facts_,
          enclosing_class_, replay_, shapes_, borrows_, conversions_,
          shape_constraints_, calls_) {}

} // namespace quidra::lowering
