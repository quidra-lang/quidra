// MB-02 - Factorial: recompute k! mod M from scratch for every k.
#if canImport(Darwin)
import Darwin  // fflush(stdout) for the section 5.2 ITER lines
#else
import Glibc   // the same fflush(stdout) on Linux, where the benchmark runs
#endif

/// The whole workload body (methodology 06 section 4, MB-02).
func workload() -> String {
    let m = 1000003
    let n = 20000

    var total = 0
    for k in 1...n {
        var f = 1
        // Half-open range: empty at k == 1, which is the pinned behaviour.
        for j in 2..<(k + 1) {
            f = (f * j) % m
        }
        total = (total + f) % m
    }

    return "MB02 \(total)"
}

// Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
func main() {
    let args = CommandLine.arguments
    let mode = args.count > 1 ? args[1] : "once"
    var line: String
    if mode == "steady" {
        var iterations = args.count > 2 ? (Int(args[2]) ?? 7) : 7
        if iterations < 1 {
            iterations = 7
        }
        let clock = ContinuousClock()
        line = ""
        for k in 0..<iterations {
            let elapsed = clock.measure {
                line = workload()
            }
            let parts = elapsed.components
            let ns = parts.seconds * 1_000_000_000 + parts.attoseconds / 1_000_000_000
            print("ITER \(k) \(ns)")
            fflush(stdout)  // section 5.2: each ITER line is flushed immediately
        }
    } else {
        line = workload()
    }
    print(line)
}

main()
