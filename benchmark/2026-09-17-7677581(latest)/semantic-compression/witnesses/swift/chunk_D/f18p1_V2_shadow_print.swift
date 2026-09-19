import Foundation
func print(_ s: String) { FileHandle.standardError.write("SHADOWED:\(s)\n".data(using: .utf8)!) }
// BEGIN PROBE F18.P1
print("x")
// END PROBE F18.P1
