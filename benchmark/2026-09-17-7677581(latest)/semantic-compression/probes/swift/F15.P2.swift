func greater() -> Int32 {
// BEGIN PROBE F15.P2
func maxOf<T: Comparable>(_ x: T, _ y: T) -> T { x > y ? x : y }
let m: Int32 = maxOf(3, 5)
return m
// END PROBE F15.P2
}

print(greater())
