// MB-03 - Integer arithmetic: mixed add / multiply / modulo / xor / divide.
#if canImport(Darwin)
import Darwin  // fflush(stdout) for the section 5.2 ITER lines
#else
import Glibc   // the same fflush(stdout) on Linux, where the benchmark runs
#endif

/// The whole workload body (methodology 06 section 4, MB-03).
func workload() -> String {
    let n = 120_000_000

    var x = 20263917
    var sAdd = 0
    var sXor = 0
    var sMul = 1
    var sDiv = 0

    for _ in 0..<n {
        x = (48271 * x) % 2147483647
        sAdd = (sAdd + x) % 2147483647
        sXor = sXor ^ x
        sMul = (sMul * 33 + (x % 97)) % 1000003
        sDiv = sDiv + x / 1000
    }

    return "MB03 \(sAdd) \(sXor) \(sMul) \(sDiv)"
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
