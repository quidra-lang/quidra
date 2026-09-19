func second(_ xs: [Int]) -> Int {
// BEGIN PROBE F11.P2
let part = xs[1..<4]
return part.first!
// END PROBE F11.P2
}

print(second([10, 20, 30, 40, 50]))
