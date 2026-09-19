// MB-10 - File I/O: buffered line-by-line text write, then buffered read-back and parse.
//
// Section 4.12(c) pins the writer and reader for the `swift` configuration: a manual
// 65536-byte `[UInt8]` buffer flushed with `FileHandle.write(contentsOf:)`, and
// 65536-byte chunks read with `FileHandle.read(upToCount:)` with carry-over line
// splitting. Swift ships no buffered text writer and no streaming line reader of its
// own, which is why the table names the manual-buffer shape here (the same shape it
// names for Node). Foundation is standard library, not a third-party dependency.
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
}

/// Section 4.12(c): one frozen 65536-byte buffer for every configuration.
let bufSize = 65536

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data(("mb10: " + message + "\n").utf8))
    exit(1)
}

/// Parse one decimal field out of the UTF-8 bytes of a line.
func parseDecimal(_ field: ArraySlice<UInt8>) -> Int? {
    if field.isEmpty {
        return nil
    }
    var value = 0
    for b in field {
        let digit = Int(b) - 48
        if digit < 0 || digit > 9 {
            return nil
        }
        value = value * 10 + digit
    }
    return value
}

/// The whole workload body (methodology 06 section 4, MB-10). Section 5.2 requires
/// each steady iteration to re-seed the generator and rewrite its input files.
func workload() -> String {
    let n = 1000000
    let r = 3

    var gen = Lcg(seed: 20270917)  // the stream continues across rounds
    var sumV = 0
    var chk = 0
    var nbytes = 0
    var lines = 0

    var wbuf = [UInt8]()
    wbuf.reserveCapacity(bufSize + 64)
    var carry = [UInt8]()
    carry.reserveCapacity(64)

    for round in 0..<r {
        let name = "mb10_round_\(round).txt"

        FileManager.default.createFile(atPath: name, contents: nil)
        guard let out = FileHandle(forWritingAtPath: name) else {
            fail("cannot open \(name) for writing")
        }
        wbuf.removeAll(keepingCapacity: true)
        do {
            for i in 0..<n {
                let v = gen.nextInt()
                let line = "\(i) \(v)\n"
                let before = wbuf.count
                wbuf.append(contentsOf: line.utf8)
                nbytes += wbuf.count - before
                if wbuf.count >= bufSize {
                    try out.write(contentsOf: wbuf)
                    wbuf.removeAll(keepingCapacity: true)
                }
            }
            if !wbuf.isEmpty {
                try out.write(contentsOf: wbuf)
                wbuf.removeAll(keepingCapacity: true)
            }
            try out.close()
        } catch {
            fail("write failed on \(name): \(error)")
        }

        guard let input = FileHandle(forReadingAtPath: name) else {
            fail("cannot open \(name) for reading")
        }
        var idx = 0
        carry.removeAll(keepingCapacity: true)
        do {
            while let data = try input.read(upToCount: bufSize), !data.isEmpty {
                let chunk = [UInt8](data)
                var start = 0
                while let nl = chunk[start...].firstIndex(of: 10) {
                    var a: Int?
                    var v: Int?
                    if carry.isEmpty {
                        (a, v) = splitLine(chunk[start..<nl])
                    } else {
                        carry.append(contentsOf: chunk[start..<nl])
                        (a, v) = splitLine(carry[...])
                        carry.removeAll(keepingCapacity: true)
                    }
                    guard let aVal = a, let vVal = v else {
                        fail("malformed line in \(name)")
                    }
                    if aVal != idx {
                        fail("index mismatch in \(name): got \(aVal), expected \(idx)")
                    }
                    idx += 1
                    lines += 1
                    sumV = (sumV + vVal) % 1000000007
                    chk = (chk * 31 + (vVal % 1000003)) % 1000003
                    start = nl + 1
                }
                if start < chunk.count {
                    carry.append(contentsOf: chunk[start...])
                }
            }
            try input.close()
        } catch {
            fail("read failed on \(name): \(error)")
        }
        if !carry.isEmpty {
            fail("unterminated final line in \(name)")
        }
    }

    return "MB10 \(sumV) \(chk) \(nbytes) \(lines)"
}

/// Split a line at its single space into two decimal fields.
func splitLine(_ line: ArraySlice<UInt8>) -> (Int?, Int?) {
    guard let sp = line.firstIndex(of: 32) else {
        return (nil, nil)
    }
    return (parseDecimal(line[line.startIndex..<sp]), parseDecimal(line[(sp + 1)...]))
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
