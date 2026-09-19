final class Node {
    var id: Int32
    init(id: Int32) { self.id = id }
}

func probe() -> Bool {
// BEGIN PROBE F04.P3
let first = Node(id: 5)
let second = [first][0]
let same = first === second
return same
// END PROBE F04.P3
}

print(probe())
