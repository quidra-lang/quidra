// MB-04 - Floating-point arithmetic: four accumulators over two arrays.
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

/// The whole workload body, generation included (methodology 06 section 4, MB-04).
/// Section 5.2 requires each steady iteration to re-seed and regenerate its input,
/// so the generator and both arrays are built here rather than hoisted out.
func workload() -> String {
    let m = 4000
    let r = 75000

    var gen = Lcg(seed: 20264917)
    var a = [Double](repeating: 0.0, count: m)
    var b = [Double](repeating: 0.0, count: m)
    for i in 0..<m {
        a[i] = 0.5 + gen.nextUnit()
    }
    for i in 0..<m {
        b[i] = 0.5 + gen.nextUnit()
    }

    var s1 = 0.0
    var s2 = 0.0
    var s3 = 0.0
    var s4 = 0.0
    for round in 0..<r {
        a[round % m] = a[round % m] + 1.0e-9  // anti-elimination, part of the algorithm
        for i in 0..<m {
            let av = a[i]
            let bv = b[i]
            s1 = s1 + av * bv
            s2 = s2 + av / (bv + 2.0)
            s3 = s3 + (av * av + bv * bv).squareRoot()
            s4 = s4 + (av - bv) * (av - bv)
        }
    }

    return String(format: "MB04 s1=%.16e s2=%.16e s3=%.16e s4=%.16e", s1, s2, s3, s4)
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
