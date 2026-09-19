func probe() -> Int32 {
// BEGIN PROBE F05.P2
func put(_ target: inout Int32) {
    target = 12
}
var cell: Int32 = 3
put(&cell)
return cell
// END PROBE F05.P2
}

print(probe())
