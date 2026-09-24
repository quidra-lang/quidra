import Foundation
print("ADV-START"); fflush(stdout)
var xs: [Int64] = [1, 2, 3, 4, 5]
var iters = 0
for x in xs {
    iters += 1
    if x == 2 {
        xs.append(99)
    }
}
print("OBS=ITERS:\(iters)|LEN:\(xs.count)"); fflush(stdout)
print("ADV-END"); fflush(stdout)
