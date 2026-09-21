// MB-08 - Strings: build a word text, then five character-level passes per round.
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

/// Order-sensitive rolling hash over an ASCII byte sequence.
func rhash(_ seq: [UInt8]) -> Int {
    var h = 0
    for c in seq {
        h = (h * 131 + Int(c)) % 1000000007
    }
    return h
}

/// The whole workload body, generation and join included (methodology 06 section 4,
/// MB-08). Section 5.2 requires each steady iteration to re-seed and regenerate its
/// input, so the build phase lives inside the body.
func workload() -> String {
    let nw = 200000
    let r = 20
    let q = 1000000007

    var gen = Lcg(seed: 20268917)
    var words = [String]()
    for _ in 0..<nw {
        let l = 4 + (gen.nextInt() % 13)
        var letters = [UInt8]()
        for _ in 0..<l {
            letters.append(UInt8(97 + gen.nextInt() % 26))
        }
        words.append(String(decoding: letters, as: UTF8.self))
    }
    // Passes 1-4 operate on the pinned `[UInt8]` working buffer (section 4.12(b)).
    // The text is ASCII, so the bytes of the joined string are its characters.
    var text = Array(words.joined(separator: " ").utf8)
    let len = text.count

    var acc = 0
    var cntAb = 0
    var cntW = 0
    for round in 0..<r {
        let p = 7 * round + 11  // anti-elimination, part of the algorithm
        if text[p] == 32 {
            text[p] = 120
        } else {
            text[p] = UInt8(97 + (Int(text[p]) - 97 + 1) % 26)
        }

        let h1 = rhash(text)                                   // pass 1

        var u = [UInt8](repeating: 0, count: len)              // pass 2: upper-case
        for i in 0..<len {
            let c = text[i]
            u[i] = (c >= 97 && c <= 122) ? c - 32 : c
        }
        let h2 = rhash(u)

        var v = [UInt8](repeating: 0, count: len)              // pass 3: reverse
        for i in 0..<len {
            v[i] = text[len - 1 - i]
        }
        let h3 = rhash(v)

        cntAb = 0                                              // pass 4: naive search
        for i in 0..<(len - 1) where text[i] == 97 && text[i + 1] == 98 {
            cntAb += 1
        }

        // Pass 5: word count on Swift's own String type, constructed fresh from the
        // working buffer inside the round and read with the section 4.12(b) pinned
        // access API for swift -- `for c in s`. Swift offers no integer index into a
        // String, so in-order Character iteration is the ordinary spelling here.
        let s = String(decoding: text, as: UTF8.self)
        cntW = 1
        for c in s where c == " " {
            cntW += 1
        }

        for value in [h1, h2, h3, cntAb, cntW] {
            acc = (acc * 31 + value) % q
        }
    }

    return "MB08 \(acc) \(len) \(cntAb) \(cntW)"
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
