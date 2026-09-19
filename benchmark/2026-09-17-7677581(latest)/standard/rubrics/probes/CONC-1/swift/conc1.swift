// CONC-1, Swift. Worker mechanism: Dispatch (libdispatch), a first-party Swift
// toolchain library: DispatchQueue.global().async inside a DispatchGroup.
import Foundation
import Dispatch

let N: Int = 500000000
let CHUNKS: Int = 4
let SPAN: Int = N / CHUNKS

func chunkSum(_ c: Int) -> Double {
    var s = 0.0
    let start = c * SPAN
    let end = start + SPAN
    var i = start
    while i < end { let x = Foundation.sin(Double(i)); s += x * x; i += 1 }
    return s
}

let W: Int = CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1])! : 1
let partial = UnsafeMutablePointer<Double>.allocate(capacity: CHUNKS)
partial.initialize(repeating: 0.0, count: CHUNKS)
let group = DispatchGroup()
for w in 0..<W {
    DispatchQueue.global().async(group: group) {
        for c in 0..<CHUNKS where c % W == w { partial[c] = chunkSum(c) }
    }
}
group.wait()
var total = 0.0
for c in 0..<CHUNKS { total = total + partial[c] }
print(String(format: "workers=%d result=%.10f", W, total))
