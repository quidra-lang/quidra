var kept: UnsafeMutablePointer<Int>? = nil
func f(_ v: UnsafePointer<Int>) { kept = UnsafeMutablePointer(mutating: v) }

func probe() -> Int {
let x = [1, 2, 3]
f(x)
return x[0]
}
print(probe())
