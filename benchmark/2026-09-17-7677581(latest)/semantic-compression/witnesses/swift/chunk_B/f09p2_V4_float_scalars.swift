var xs = [3.0, 1.0, 2.0]
let a = Double.nan
let b = 2.0
// BEGIN PROBE F09.P2
xs.sort(by: >)
let lt = a < b
// END PROBE F09.P2
print(xs, lt, b < a)
