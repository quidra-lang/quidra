func combine() -> String {
// BEGIN PROBE F15.P1
func head<T>(_ xs: [T]) -> T { xs[0] }
let a = head([4, 5, 6])
let b = head(["p", "q"])
return String(a) + b
// END PROBE F15.P1
}

print(combine())
