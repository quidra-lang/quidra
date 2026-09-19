var saved: [Int] = []
func f(_ v: [Int]) { saved = v }

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}

let r = probe()
saved[0] = 77
print("result=\(r) savedAfterWrite=\(saved[0])")
