import Foundation
func main() {
    print("ADV-START"); fflush(stdout)
    let s: Int32 = -1
    let u: UInt32 = 1
    let v = s < u
    print("OBS=CMP:\(v)"); fflush(stdout)
    print("ADV-END"); fflush(stdout)
}
main()
