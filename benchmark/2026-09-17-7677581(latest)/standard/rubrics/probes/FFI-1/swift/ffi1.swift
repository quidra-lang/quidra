import Foundation   // re-exports Darwin; the C standard library reaches Swift through first-party C interop
print(String(format: "cos(1.0)=%.10f", cos(1.0)))
