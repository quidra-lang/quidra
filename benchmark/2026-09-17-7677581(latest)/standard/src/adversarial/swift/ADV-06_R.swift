import Foundation
func main() {
    print("ADV-START"); fflush(stdout)
    let a = Double(readLine()!)!
    let b = Double(readLine()!)!
    let m = a / b
    let xs: [Double] = [3.0, m, 1.0]
    var best = xs[0]
    for x in xs[1...] {
        if x > best { best = x }
    }
    let selfeq = (m == m)
    print("OBS=MAX:\(String(format: "%.6f", best))|SELFEQ:\(selfeq)"); fflush(stdout)
    print("ADV-END"); fflush(stdout)
}
main()
