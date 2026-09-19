var state: Int = 7
var total: Int = 0
var largest: Int = 0
var evens: Int = 0
var joined: String = ""
for i in 0..<50 {
    state = (state * 48271) % 2147483647
    let term: Int = state % 1000
    total = total + term
    if term > largest {
        largest = term
    }
    if term % 2 == 0 {
        evens = evens + 1
    }
    if i < 5 {
        if i > 0 {
            joined = joined + "-"
        }
        joined = joined + "\(term)"
    }
}
print("SUM \(total)")
print("MAX \(largest)")
print("EVENS \(evens)")
print("JOINED \(joined)")
