func probe(_ start: Double) -> Double {
    var x: Double = start
x += 1
    return x
}
print("v2 result=\(probe(41)) edge=\(probe(Double.greatestFiniteMagnitude) == Double.greatestFiniteMagnitude)")
