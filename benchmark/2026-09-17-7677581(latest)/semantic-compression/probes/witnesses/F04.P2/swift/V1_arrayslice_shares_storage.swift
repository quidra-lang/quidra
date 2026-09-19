func probe() -> String {
    let xs = [7, 8, 9]
let window = xs[...]
    let a = xs.withUnsafeBufferPointer { UInt(bitPattern: $0.baseAddress) }
    let b = window.withUnsafeBufferPointer { UInt(bitPattern: $0.baseAddress) }
    return "shared=\(a == b) e0=\(window[0]) type=\(type(of: window))"
}
print("v1 " + probe())
