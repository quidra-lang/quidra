import Foundation
struct FileHandle {
    init(forReadingFrom url: URL) throws {}
    func close() throws {}
    func readToEnd() throws -> Data? { "SHADOWED".data(using: .utf8) }
}
// BEGIN PROBE F17.P1
func readAll() throws -> Int {
    let handle = try FileHandle(forReadingFrom: URL(filePath: "data.txt"))
    defer { try? handle.close() }
    let text = String(decoding: try handle.readToEnd() ?? Data(), as: UTF8.self)
    return text.utf8.count
}
// END PROBE F17.P1
print(try readAll())
