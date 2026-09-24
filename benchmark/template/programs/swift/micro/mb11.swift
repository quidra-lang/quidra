// MB-11 - Collections: Swift's own Dictionary, Set and Array under
// insert / update / lookup / delete / iterate. Default capacity everywhere.
// Section 4.12(d) pins Dictionary<Int,Int>, Set<Int> and Array<Int> for swift.
#if canImport(Darwin)
import Darwin  // fflush(stdout) for the section 5.2 ITER lines
#else
import Glibc   // the same fflush(stdout) on Linux, where the benchmark runs
#endif

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
}

/// The whole workload body, generation included (methodology 06 section 4, MB-11).
/// Section 5.2 requires each steady iteration to re-seed and regenerate `keys`.
func workload() -> String {
    let n = 1000000
    let r = 3
    let km = 500009
    let sm = 100003
    let q = 1000000007

    var gen = Lcg(seed: 20271917)
    var keys = [Int](repeating: 0, count: n)
    for i in 0..<n {
        keys[i] = gen.nextInt()
    }

    var acc = 0
    var size1 = 0
    var found = 0
    var vsum = 0
    var mchk = 0
    var size2 = 0
    var size3 = 0
    var lsum = 0

    for round in 0..<r {
        keys[round] = keys[round] + 1000000  // anti-elimination, stays below 2^31

        var m = [Int: Int]()
        for i in 0..<n {
            let k = keys[i] % km
            m[k] = (m[k] ?? 0) + 1
        }
        size1 = m.count

        found = 0
        vsum = 0
        for i in 0..<n {
            let k = (keys[i] + 7) % km
            if let v = m[k] {
                found += 1
                vsum += v
            }
        }

        mchk = 0
        for (k, v) in m {  // commutative, so map iteration order does not matter
            mchk = (mchk + (k % 1000003) * v) % 1000003
        }

        for i in stride(from: 0, to: n, by: 2) {
            m.removeValue(forKey: keys[i] % km)
        }
        size2 = m.count

        var st = Set<Int>()
        for i in 0..<n {
            st.insert(keys[i] % sm)
        }
        size3 = st.count

        var lst = [Int]()
        for i in 0..<n {
            lst.append(keys[i] % 1000)
        }
        lsum = 0
        for v in lst {
            lsum += v
        }

        for value in [size1, found, vsum, mchk, size2, size3, lsum] {
            acc = (acc * 31 + value) % q
        }
    }

    return "MB11 \(acc) \(size1) \(found) \(vsum) \(mchk) \(size2) \(size3) \(lsum)"
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
