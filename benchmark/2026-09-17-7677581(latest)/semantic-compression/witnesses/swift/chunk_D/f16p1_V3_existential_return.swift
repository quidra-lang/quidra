func sumSequence() -> Any {
// BEGIN PROBE F16.P1
let xs: [Int32] = [1, 2, 3]
let total = xs.reduce(0, +)
return total
// END PROBE F16.P1
}
print(type(of: sumSequence()), sumSequence())
