import Foundation
func main() {
    print("ADV-START"); fflush(stdout)
    let s = Int32(readLine()!)!
    let u = UInt32(readLine()!)!
    let v = s < u
    print("OBS=CMP:\(v)"); fflush(stdout)
    print("ADV-END"); fflush(stdout)
}
main()
