extension Array where Element == Int {
    mutating func sort(by areInIncreasingOrder: (Int, Int) -> Bool) { print("user sort"); self = [9, 9, 9] }
}
var xs = [3, 1, 2]
let a: Int32 = 1
let b: Int32 = 2
// BEGIN PROBE F09.P2
xs.sort(by: >)
let lt = a < b
// END PROBE F09.P2
print(xs, lt)
