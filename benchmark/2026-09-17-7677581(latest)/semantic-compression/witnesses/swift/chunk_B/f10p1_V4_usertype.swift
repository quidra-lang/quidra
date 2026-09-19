struct Tag: ExpressibleByIntegerLiteral {
    var v: Swift.Int
    init(integerLiteral value: Swift.Int) { v = value }
    init?(exactly src: Swift.Int64) { print("user exactly"); return nil }
}
typealias Int32 = Tag

func narrow(_ big: Swift.Int64) -> Int32 {
// BEGIN PROBE F10.P1
let small = Int32(exactly: big) ?? 0
return small
// END PROBE F10.P1
}

print(narrow(5).v)
