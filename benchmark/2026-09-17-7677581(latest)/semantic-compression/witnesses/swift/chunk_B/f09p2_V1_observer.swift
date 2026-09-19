var xs = [3, 1, 2]
let alias = xs
let a: Int32 = 1
let b: Int32 = 2
let capBefore = xs.capacity
// BEGIN PROBE F09.P2
xs.sort(by: >)
let lt = a < b
// END PROBE F09.P2
print(xs, lt, alias, capBefore, xs.capacity)
