extension Int32 {
    init?(_ s: String) { self = 7 }
}
func parseTwice(_ s: String) -> Int32? { Int32(s).map { $0 * 2 } }

print(parseTwice("21")!)
