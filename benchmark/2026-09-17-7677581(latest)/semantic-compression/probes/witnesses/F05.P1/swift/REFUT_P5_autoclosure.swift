func f(_ v: @autoclosure () -> [Int]) {
    print("autoclosure, not evaluated")
}

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}

print(probe())
