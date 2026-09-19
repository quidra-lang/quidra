func probe(cond: Bool) -> Int32 {
    let v: Int32
    if cond {
        v = 5
    }
    return v
}
print(probe(cond: true))
