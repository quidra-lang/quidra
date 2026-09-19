// MB-07 - Sorting: bottom-up iterative merge sort, ascending, stable, ping-pong buffers.
import Darwin  // fflush(stdout) for the section 5.2 ITER lines

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

/// One bottom-up merge pass of the pinned algorithm: merge runs of `width`
/// from `src` into `dst`.
func mergePass(_ src: inout [Int], _ dst: inout [Int], _ n: Int, _ width: Int) {
    var lo = 0
    while lo < n {
        let mid = min(lo + width, n)
        let hi = min(lo + 2 * width, n)
        var i = lo
        var j = mid
        var k = lo
        while i < mid && j < hi {
            if src[i] <= src[j] {
                dst[k] = src[i]
                i += 1
            } else {
                dst[k] = src[j]
                j += 1
            }
            k += 1
        }
        while i < mid {
            dst[k] = src[i]
            i += 1
            k += 1
        }
        while j < hi {
            dst[k] = src[j]
            j += 1
            k += 1
        }
        lo += 2 * width
    }
}

/// Bottom-up merge sort of `a[0..<n]`; `buf` is the ping-pong partner buffer.
/// The source/destination roles alternate, exactly as the pinned pseudocode
/// specifies, and the mandated final copy-back runs when an odd number of
/// passes has left the sorted data in `buf` (21 passes at n = 2000000).
func msort(_ a: inout [Int], _ buf: inout [Int], _ n: Int) {
    var flipped = false
    var width = 1
    while width < n {
        if flipped {
            mergePass(&buf, &a, n, width)
        } else {
            mergePass(&a, &buf, n, width)
        }
        flipped = !flipped
        width *= 2
    }
    if flipped {
        // "if src is not a: copy src[0..n-1] into a[0..n-1]"
        a.replaceSubrange(0..<n, with: buf[0..<n])
    }
}

/// The whole workload body, generation included (methodology 06 section 4, MB-07).
/// Section 5.2 requires each steady iteration to re-seed and regenerate its input.
func workload() -> String {
    let n = 2000000
    let r = 4

    var gen = Lcg(seed: 20267917)
    var src = [Int](repeating: 0, count: n)
    for i in 0..<n {
        src[i] = gen.nextInt()
    }
    var buf = [Int](repeating: 0, count: n)

    var total = 0
    var ssum = 0
    var inv = 0
    for round in 0..<r {
        src[round] = src[round] + 1  // anti-elimination, part of the algorithm
        var a = src                  // independent copy of the unsorted input
        msort(&a, &buf, n)

        var chk = 0
        for i in 0..<n {
            chk = (chk * 31 + (a[i] % 1000003)) % 1000003
        }
        total = (total * 7 + chk) % 1000003

        ssum = 0
        for i in 0..<n {
            ssum = ssum + a[i]
        }
        for i in 1..<n where a[i - 1] > a[i] {
            inv += 1
        }
    }

    return "MB07 \(total) \(ssum) \(inv)"
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
