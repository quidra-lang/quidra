import Foundation
print("ADV-START"); fflush(stdout)
var xs: [Int64] = []
for _ in 0..<7 {
    xs.append(Int64(readLine()!)!)
}
let target = Int64(readLine()!)!
var lo = 0
var hi = 6
var result = -1
while lo <= hi {
    let mid = (lo + hi) / 2
    if xs[mid] == target {
        result = mid
        break
    } else if xs[mid] < target {
        lo = mid + 1
    } else {
        hi = mid - 1
    }
}
print("OBS=IDX:\(result)"); fflush(stdout)
print("ADV-END"); fflush(stdout)
