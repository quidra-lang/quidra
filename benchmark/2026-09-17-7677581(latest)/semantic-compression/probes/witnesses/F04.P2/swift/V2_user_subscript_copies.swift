struct Copying {
    var store = [7, 8, 9]
    subscript(r: UnboundedRange) -> [Int] { store.map { $0 } }
}
func probe() -> String {
    let xs = Copying()
let window = xs[...]
    let a = xs.store.withUnsafeBufferPointer { UInt(bitPattern: $0.baseAddress) }
    let b = window.withUnsafeBufferPointer { UInt(bitPattern: $0.baseAddress) }
    return "shared=\(a == b) e0=\(window[0]) type=\(type(of: window))"
}
print("v2 " + probe())
