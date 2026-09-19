import Foundation
try! "hello".write(toFile: "x18.txt", atomically: true, encoding: .utf8)
let d = try! String(contentsOfFile: "x18.txt", encoding: .utf8)
print("X18", d, FileManager.default.fileExists(atPath: "x18.txt"))
