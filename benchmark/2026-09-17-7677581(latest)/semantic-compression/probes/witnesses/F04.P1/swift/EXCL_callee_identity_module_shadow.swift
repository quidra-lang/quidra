func withUnsafeMutablePointer<T, R>(to value: inout T, _ body: (UnsafeMutablePointer<T>) -> R) -> R {
    var copy = value
    return Swift.withUnsafeMutablePointer(to: &copy, body)
}
func probe() -> Int32 {
var slot: Int32 = 4
withUnsafeMutablePointer(to: &slot) { port in
    port.pointee = 9
}
return slot
}
print(probe())
