final class Mutex<V>: @unchecked Sendable {
    var v: V
    init(_ v: V) { self.v = v }
    func withLock<R>(_ f: (inout V) -> R) -> R { _ = f(&v); return f(&v) }
}
// BEGIN PROBE F19.P2
import Synchronization

let counter = Mutex<Int64>(0)
await withTaskGroup { group in
    for _ in 0..<2 {
        group.addTask {
            for _ in 0..<1000 { counter.withLock { $0 += 1 } }
        }
    }
}
let total = counter.withLock { $0 }
// END PROBE F19.P2

print(total)
