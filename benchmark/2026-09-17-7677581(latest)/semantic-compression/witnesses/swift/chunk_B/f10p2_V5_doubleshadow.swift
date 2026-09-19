typealias Double = Float

let i: Int32 = 3
let d: Double = 0.5
// BEGIN PROBE F10.P2
let sum = Double(i) + d
// END PROBE F10.P2
print(sum, MemoryLayout.size(ofValue: sum))
