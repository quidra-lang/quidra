func mapTotal() -> Int32 {
// BEGIN PROBE F16.P2
let mp: [String: Int32] = ["a": 1]
var total: Int32 = 0
for e in mp { total += e.value }
let miss = mp["b"] ?? 0
return total + miss
// END PROBE F16.P2
}

print(mapTotal())
