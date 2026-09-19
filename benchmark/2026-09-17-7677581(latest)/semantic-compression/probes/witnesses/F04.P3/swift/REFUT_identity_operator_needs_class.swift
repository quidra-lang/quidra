struct SNode { var id: Int32 }
func probe() -> Bool {
    let first = SNode(id: 5)
    let second = [first][0]
    let same = first === second
    return same
}
print(probe())
