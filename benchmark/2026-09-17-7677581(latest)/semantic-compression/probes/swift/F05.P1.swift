func f(_ v: [Int]) {
}

func probe() -> Int {
// BEGIN PROBE F05.P1
let x = [1, 2, 3]
f(x)
return x[0]
// END PROBE F05.P1
}

print(probe())
