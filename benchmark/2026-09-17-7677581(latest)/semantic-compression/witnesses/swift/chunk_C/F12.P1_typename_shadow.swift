struct Int32: ExpressibleByNilLiteral, ExpressibleByIntegerLiteral {
    var v: Int
    init(nilLiteral: ()) { v = -1 }
    init(integerLiteral value: Int) { v = value }
}
func fallback() -> Int32 {
let o: Int32? = nil
let n: Int32 = o ?? 0
return n
}
print(fallback().v)
