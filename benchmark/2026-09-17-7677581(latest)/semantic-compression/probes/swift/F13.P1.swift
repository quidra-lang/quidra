// BEGIN PROBE F13.P1
func parseTwice(_ s: String) -> Int32? { Int32(s).map { $0 * 2 } }
// END PROBE F13.P1

print(parseTwice("21")!)
