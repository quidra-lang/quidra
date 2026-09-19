func probe(cond: Bool) -> Int32 {
// BEGIN PROBE F02.P1
let v: Int32
if cond {
    v = 5
} else {
    v = 9
}
return v
// END PROBE F02.P1
}

print(probe(cond: true))
