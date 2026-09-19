struct Upto: Sequence, IteratorProtocol { var i = 0; let n: Int
  mutating func next() -> Int? { if i < n { defer { i += 1 }; return i }; return nil } }
var out: [String] = []
for v in Upto(n: 3) { out.append(String(v)) }
print("X07", out.joined(separator: " "))
