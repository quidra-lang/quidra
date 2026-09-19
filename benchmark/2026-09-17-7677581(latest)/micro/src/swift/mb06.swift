// MB-06 - Matrix multiplication: classical i-j-k order over flat row-major storage.
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

/// The whole workload body, generation included (methodology 06 section 4, MB-06).
/// Section 5.2 requires each steady iteration to re-seed and regenerate its input,
/// so the generator and the matrices are built here rather than hoisted out.
func workload() -> String {
    let n = 512
    let r = 3

    var gen = Lcg(seed: 20266917)
    var a = [Double](repeating: 0.0, count: n * n)
    var b = [Double](repeating: 0.0, count: n * n)
    var c = [Double](repeating: 0.0, count: n * n)
    for i in 0..<(n * n) {
        a[i] = gen.nextUnit()
    }
    for i in 0..<(n * n) {
        b[i] = gen.nextUnit()
    }

    for round in 0..<r {
        a[round] = a[round] + 1.0e-9  // anti-elimination, part of the algorithm
        for i in 0..<n {
            for j in 0..<n {
                var s = 0.0
                for k in 0..<n {
                    s = s + a[i * n + k] * b[k * n + j]
                }
                c[i * n + j] = s
            }
        }
    }

    var sumC = 0.0
    for i in 0..<(n * n) {
        sumC = sumC + c[i]
    }

    return String(
        format: "MB06 sumC=%.16e c_first=%.16e c_last=%.16e", sumC, c[0], c[n * n - 1])
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
