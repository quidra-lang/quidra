var log: [String] = []
struct Src {
    func map<T>(_ f: (Int32) -> T) -> [T] {
        log.append("user map ran, callback never called")
        return [f(0)]
    }
}
func probe() -> Int32 {
    let xs = Src()
    let k: Int32 = 0
let neg = { (a: Int32) in k - a }
let ys = xs.map(neg)
return ys[0]
}
print("result=\(probe()) log=\(log)")
