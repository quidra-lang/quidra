import Foundation
func main() {
    print("ADV-START"); fflush(stdout)
    let n = Int64(readLine()!)!
    var xs = [Int64](repeating: 0, count: Int(n))
    xs[0] = 1
    let first = xs[0]
    print("OBS=ALLOC:\(xs.count)|FIRST:\(first)"); fflush(stdout)
    print("ADV-END"); fflush(stdout)
}
main()
