let p = UnsafeMutablePointer<Int>.allocate(capacity: 8)
p.initialize(repeating: 0, count: 8)
for k in 0..<8 { p[k] = k * 100 }
let a = 1
let b = 2
let c = p
// BEGIN PROBE F07.P1
let r = a * b + c
// END PROBE F07.P1
print(r.pointee)
