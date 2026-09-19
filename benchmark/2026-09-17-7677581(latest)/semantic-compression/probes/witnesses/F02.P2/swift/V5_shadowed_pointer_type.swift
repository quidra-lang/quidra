struct UnsafeMutablePointer<T> {
    var store: [T]
    static func allocate(capacity: Int) -> UnsafeMutablePointer<T> where T: ExpressibleByIntegerLiteral {
        UnsafeMutablePointer(store: Array(repeating: 0, count: capacity))
    }
    func initialize(to v: T) { }
    subscript(i: Int) -> T { store[i] }
}
func probe() -> UInt8 {
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 16)
buf.initialize(to: 1)
return buf[0]
}
print("value=\(probe()) noAllocationNoInitialize=true")
