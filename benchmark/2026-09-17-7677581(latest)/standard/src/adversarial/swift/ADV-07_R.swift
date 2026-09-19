import Foundation
func main() {
    print("ADV-START"); fflush(stdout)
    let a = Double(readLine()!)!
    let b = Double(readLine()!)!
    let h = a / b
    let mean = (1.0 + h + 3.0) / 3.0
    let diff = h - h
    print("OBS=MEAN:\(String(format: "%.6f", mean))|DIFF:\(String(format: "%.6f", diff))"); fflush(stdout)
    print("ADV-END"); fflush(stdout)
}
main()
