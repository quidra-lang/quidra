typealias IntegerLiteralType = UnsafeRawPointer
func probe() -> UnsafeRawPointer {
let n = 7
return n
}
print(probe())
