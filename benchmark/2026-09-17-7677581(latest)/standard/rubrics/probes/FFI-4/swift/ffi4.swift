import Foundation   // re-exports Darwin; the C standard library reaches Swift through first-party C interop
// Declarations come from probes/FFI-4/c/supplied.h via -import-objc-header; none are hand-written here.
var r = SuppliedRecord(count: 3, weight: 1.5)
print("supplied_add=\(supplied_add(20, 22))")
print(String(format: "supplied_weighted=%.2f", supplied_weighted(&r)))
print("SUPPLIED_SCALE=\(SUPPLIED_SCALE)")
