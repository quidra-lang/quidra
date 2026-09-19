func probe() -> String {
func put(_ target: inout Int32) {
    target = 12
}
var cell: Int32 = 3
put(&cell)
return "cell=\(cell) writtenByCallee=\(cell == 12)"
}
print(probe())
