func f(_ v: inout [Int]) { v = [99] }

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}
print(probe())
