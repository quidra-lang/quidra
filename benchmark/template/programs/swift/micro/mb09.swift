// MB-09 - Statistics: two-pass moments, Pearson correlation, histogram.
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

/// The whole workload body, generation included (methodology 06 section 4, MB-09).
/// Section 5.2 requires each steady iteration to re-seed and regenerate X and Y.
func workload() -> String {
    let n = 2000000
    let r = 30

    var gen = Lcg(seed: 20269917)
    var x = [Double](repeating: 0.0, count: n)
    var y = [Double](repeating: 0.0, count: n)
    for i in 0..<n {
        x[i] = gen.nextUnit() * 100.0
    }
    for i in 0..<n {
        y[i] = gen.nextUnit() * 100.0
    }

    var mean = 0.0
    var variance = 0.0
    var sd = 0.0
    var mn = 0.0
    var mx = 0.0
    var mad = 0.0
    var pearson = 0.0
    var histChk = 0

    for round in 0..<r {
        x[round] = x[round] + 1.0e-9  // anti-elimination, part of the algorithm

        var s = 0.0                                            // pass 1
        mn = x[0]
        mx = x[0]
        for i in 0..<n {
            let v = x[i]
            s = s + v
            if v < mn { mn = v }
            if v > mx { mx = v }
        }
        mean = s / Double(n)

        var sq = 0.0                                           // pass 2
        var ad = 0.0
        for i in 0..<n {
            let d = x[i] - mean
            sq = sq + d * d
            ad = ad + (d < 0 ? -d : d)
        }
        variance = sq / Double(n)
        sd = variance.squareRoot()
        mad = ad / Double(n)

        var sy = 0.0                                           // pass 3
        for i in 0..<n {
            sy = sy + y[i]
        }
        let meany = sy / Double(n)

        var sxy = 0.0                                          // pass 4
        var sxx = 0.0
        var syy = 0.0
        for i in 0..<n {
            let dx = x[i] - mean
            let dy = y[i] - meany
            sxy = sxy + dx * dy
            sxx = sxx + dx * dx
            syy = syy + dy * dy
        }
        pearson = sxy / (sxx * syy).squareRoot()

        var hist = [Int](repeating: 0, count: 64)              // pass 5
        for i in 0..<n {
            // The fairness note pins the bin as floor(x * 0.64).
            var b = Int((x[i] * 0.64).rounded(.down))
            if b < 0 { b = 0 }
            if b > 63 { b = 63 }
            hist[b] += 1
        }
        histChk = 0
        for b in 0..<64 {
            histChk = histChk + (b + 1) * hist[b]
        }
    }

    let fields = String(
        format: "mean=%.16e var=%.16e sd=%.16e min=%.16e max=%.16e mad=%.16e pearson=%.16e",
        mean, variance, sd, mn, mx, mad, pearson)
    return "MB09 \(fields) hist_chk=\(histChk)"
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
