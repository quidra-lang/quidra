func unit(_ v: Int) async -> Int {
    withUnsafeCurrentTask { print("unit \(v) task id:", $0!.hashValue) }
    return v
}
async let first = unit(20)
async let second = unit(22)
let sum = await first + second
withUnsafeCurrentTask { print("parent task id:", $0!.hashValue) }
print(sum)
