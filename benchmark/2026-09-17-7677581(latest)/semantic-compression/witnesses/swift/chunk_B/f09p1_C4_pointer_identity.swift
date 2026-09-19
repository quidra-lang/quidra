let buf = UnsafeMutableRawPointer.allocate(byteCount: 8, alignment: 8)
buf.storeBytes(of: 7, as: Int.self)
let other = UnsafeMutableRawPointer.allocate(byteCount: 8, alignment: 8)
other.storeBytes(of: 7, as: Int.self)
let s1 = UnsafeRawPointer(buf)
let s2 = UnsafeRawPointer(other)
// BEGIN PROBE F09.P1
let eq = s1 == s2
// END PROBE F09.P1
print(eq, s1.load(as: Int.self) == s2.load(as: Int.self))
