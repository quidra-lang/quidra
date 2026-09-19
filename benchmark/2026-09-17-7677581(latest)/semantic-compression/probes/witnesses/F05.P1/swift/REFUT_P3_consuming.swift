func f(_ v: consuming [Int]) { _ = v }

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}
print(probe())
