func f(_ v: [Int]?) {
    print("optional-promoted:", v as Any)
}

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}

print(probe())
