final class Node {
    var id: Int32
    init?(id: Int32) { return nil }
}
func probe() -> Bool {
let first = Node(id: 5)
let second = [first][0]
let same = first === second
return same
}
print("optionalIdentity=\(probe())")
