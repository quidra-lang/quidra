public typealias Int32 = Swift.Int64
// BEGIN PROBE F20.P2
@_cdecl("add2")
public func add2(_ a: Int32, _ b: Int32) -> Int32 { a + b }
// END PROBE F20.P2
print(add2(4000000000, 1))
