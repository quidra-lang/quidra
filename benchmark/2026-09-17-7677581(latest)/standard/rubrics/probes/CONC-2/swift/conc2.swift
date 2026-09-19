// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
import Foundation
import Dispatch

let ITERS = 200000
var counter: Int = 0

let group = DispatchGroup()
for _ in 0..<2 {
    DispatchQueue.global().async(group: group) {
        for _ in 0..<ITERS { counter = counter + 1 }
    }
}
group.wait()
print("counter=\(counter) expected=\(2 * ITERS)")
