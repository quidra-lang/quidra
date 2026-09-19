struct Table {
    subscript(i: Int) -> Int { -1 }
}
func readAt(_ xs: Table, _ i: Int) -> Int {
let e = xs[i]
return e
}
print(readAt(Table(), 999))
