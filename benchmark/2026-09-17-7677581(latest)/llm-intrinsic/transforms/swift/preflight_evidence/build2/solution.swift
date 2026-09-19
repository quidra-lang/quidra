var seed: Int = 7
var acc: Int = 0
var peak: Int = 0
var even_count: Int = 0
var head: String = ""
for k in 1..<51 {
    seed = seed * 48271 % 2147483647
    let v: Int = seed % 1000
    acc = acc + v
    if peak < v {
        peak = v
    }
    if v % 2 != 1 {
        even_count = even_count + 1
    }
    if k <= 5 {
        if k != 1 {
            head = head + "-"
        }
        head = head + "\(v)"
    }
}
print("SUM \(acc)")
print("MAX \(peak)")
print("EVENS \(even_count)")
print("JOINED \(head)")
