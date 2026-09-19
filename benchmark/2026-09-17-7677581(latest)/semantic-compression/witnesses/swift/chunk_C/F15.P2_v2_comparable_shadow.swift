protocol Comparable {
    static func > (a: Self, b: Self) -> Bool
}
extension Int32: Comparable {
    static func > (a: Int32, b: Int32) -> Bool { true }
}
func greater() -> Int32 {
func maxOf<T: Comparable>(_ x: T, _ y: T) -> T { x > y ? x : y }
let m: Int32 = maxOf(3, 5)
return m
}
print(greater())
