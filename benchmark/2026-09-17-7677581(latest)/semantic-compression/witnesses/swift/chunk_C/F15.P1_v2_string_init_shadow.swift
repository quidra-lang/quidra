extension String {
    init(_ v: Int) { self = "Z" }
}
func combine() -> String {
func head<T>(_ xs: [T]) -> T { xs[0] }
let a = head([4, 5, 6])
let b = head(["p", "q"])
return String(a) + b
}
print(combine())
