import Foundation
let re = try! NSRegularExpression(pattern: "([0-9]{4})-([0-9]{2})")
let s = "date 2026-09-17"
let m = re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s))!
let g1 = String(s[Range(m.range(at: 1), in: s)!])
let g2 = String(s[Range(m.range(at: 2), in: s)!])
print("X21", g1, g2)
