public struct Int32 { public var v: Swift.Int32
  public static func + (a: Int32, b: Int32) -> Int32 { Int32(v: a.v &* b.v) } }
// BEGIN PROBE F20.P2
@_cdecl("add2")
public func add2(_ a: Int32, _ b: Int32) -> Int32 { a + b }
// END PROBE F20.P2
print(add2(Int32(v: 2), Int32(v: 3)).v)
