import Foundation
func slow(_ v: Int) async -> Int { Thread.sleep(forTimeInterval: 0.4); return v }
let t0 = Date()
async let first = slow(20)
async let second = slow(22)
let sum = await first + second
print(sum, Date().timeIntervalSince(t0) < 0.7 ? "CONCURRENT" : "SERIAL")
