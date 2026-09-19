import Foundation

struct Record { var count: Int32; var weight: Double }

let cmpDouble: @convention(c) (UnsafeRawPointer?, UnsafeRawPointer?) -> Int32 = { a, b in
    let x = a!.load(as: Double.self), y = b!.load(as: Double.self)
    return x > y ? 1 : (x < y ? -1 : 0)
}
let cmpRecord: @convention(c) (UnsafeRawPointer?, UnsafeRawPointer?) -> Int32 = { a, b in
    let x = a!.load(as: Record.self).count, y = b!.load(as: Record.self).count
    return x > y ? 1 : (x < y ? -1 : 0)
}

print("sizeof=\(MemoryLayout<Record>.size) offset=\(MemoryLayout<Record>.offset(of: \Record.weight)!)")

var xs: [Double] = [3.5, 1.25, 4.75, 1.5, 2.25]
xs.withUnsafeMutableBufferPointer { buf in
    qsort(buf.baseAddress!, buf.count, MemoryLayout<Double>.stride, cmpDouble)
}
print(xs.map { String(format: "%.2f", $0) }.joined(separator: " "))

var rs: [Record] = [Record(count: 3, weight: 1.5), Record(count: 1, weight: 4.0), Record(count: 2, weight: 2.5)]
rs.withUnsafeMutableBufferPointer { buf in
    qsort(buf.baseAddress!, buf.count, MemoryLayout<Record>.stride, cmpRecord)
}
print(rs.map { "\($0.count):" + String(format: "%.2f", $0.weight) }.joined(separator: " "))
