func f(_ a: Int32, _ b: Int32) -> (Int32, Int32, Double) {
// BEGIN PROBE F07.P2
let q = a / b
let m = a % b
let d = Double(a) / Double(b)
// END PROBE F07.P2
return (q, m, d)
}
print(f(-7, 0))
