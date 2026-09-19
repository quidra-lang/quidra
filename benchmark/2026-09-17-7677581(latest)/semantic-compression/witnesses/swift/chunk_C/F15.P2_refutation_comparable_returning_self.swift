protocol Comparable {
    static func > (a: Self, b: Self) -> Self
}
extension Int32: Comparable {
    static func > (a: Int32, b: Int32) -> Int32 { 42 }
}
func greater() -> Int32 {
func maxOf<T: Comparable>(_ x: T, _ y: T) -> T { x > y ? x : y }
let m: Int32 = maxOf(3, 5)
return m
}
print(greater())
