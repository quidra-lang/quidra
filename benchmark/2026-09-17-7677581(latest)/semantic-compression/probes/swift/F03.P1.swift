func probe() -> Int32 {
    var x: Int32 = 41
// BEGIN PROBE F03.P1
x += 1
// END PROBE F03.P1
    return x
}

print(probe())
