func ?? (lhs: Int32?, rhs: @autoclosure () -> Int32) -> Int32 { 99 }
func fallback() -> Int32 {
let o: Int32? = nil
let n: Int32 = o ?? 0
return n
}
print(fallback())
