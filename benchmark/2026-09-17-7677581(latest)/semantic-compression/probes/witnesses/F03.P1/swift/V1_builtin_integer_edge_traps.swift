func probe(_ start: Int32) -> Int32 {
    var x: Int32 = start
    x += 1
    return x
}
print(probe(Int32(CommandLine.arguments.count) == 1 ? Int32.max : 0))
