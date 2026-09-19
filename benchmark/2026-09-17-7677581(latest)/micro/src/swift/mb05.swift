// MB-05 - Vector inner product: streaming multiply-accumulate over two arrays.
// Foundation is imported for `String(format: "%.16e", ...)`: section 2.4 requires at
// least 17 significant decimal digits and the Swift standard library has no
// printf-style floating-point formatting.
import Foundation

/// Lehmer / MINSTD generator, frozen for every language in the suite.
struct Lcg {
    private var state: Int

    init(seed: Int) {
        state = seed
    }

    mutating func nextInt() -> Int {
        state = (48271 * state) % 2147483647
        return state
    }

    mutating func nextUnit() -> Double {
        return Double(nextInt()) / 2147483647.0
    }
}

/// The whole workload body, generation included (methodology 06 section 4, MB-05).
/// Section 5.2 requires each steady iteration to re-seed and regenerate its input,
/// so the generator and both arrays are built here rather than hoisted out.
func workload() -> String {
    let n = 2_000_000
    let r = 400

    var gen = Lcg(seed: 20265917)
    var x = [Double](repeating: 0.0, count: n)
    var y = [Double](repeating: 0.0, count: n)
    for i in 0..<n {
        x[i] = 0.5 + gen.nextUnit()
    }
    for i in 0..<n {
        y[i] = 0.5 + gen.nextUnit()
    }

    var total = 0.0
    for round in 0..<r {
        x[round] = x[round] + 1.0e-9  // anti-elimination, part of the algorithm
        var d = 0.0
        for i in 0..<n {
            d = d + x[i] * y[i]
        }
        total = total + d
    }

    return String(format: "MB05 total=%.16e", total)
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
