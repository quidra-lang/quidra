import Foundation
struct A { var v: Int64 }
struct B { var v: Int64 }
func main() {
    print("ADV-START"); fflush(stdout)
    let a = A(v: 42)
    let bridge: Any = a
    let b = bridge as! B
    print("OBS=V:\(b.v)"); fflush(stdout)
    print("ADV-END"); fflush(stdout)
}
main()
