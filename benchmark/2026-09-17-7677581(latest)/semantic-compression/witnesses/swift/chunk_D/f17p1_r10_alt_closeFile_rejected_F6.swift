import Foundation
func readAll() throws -> Int {
    let handle = try FileHandle(forReadingFrom: URL(filePath: "data.txt"))
    defer { handle.closeFile() }
    let text = String(decoding: try handle.readToEnd() ?? Data(), as: UTF8.self)
    return text.utf8.count
}
print(try readAll())
