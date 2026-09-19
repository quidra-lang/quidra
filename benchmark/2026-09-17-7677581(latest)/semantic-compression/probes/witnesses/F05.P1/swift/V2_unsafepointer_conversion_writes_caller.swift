func f(_ v: UnsafePointer<Int>) {
    print("callee sees element0 =", v[0])
    UnsafeMutablePointer(mutating: v)[0] = 99
}

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}

print(probe())
