typealias Int32 = UInt8
func probe(cond: Bool) -> Int32 {
let v: Int32
if cond {
    v = 5
} else {
    v = 9
}
return v
}
print("type=\(type(of: probe(cond: true))) value=\(probe(cond: true))")
