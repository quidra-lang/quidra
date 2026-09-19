var released: Int32 = 0

// BEGIN PROBE F17.P2
class Handle {
    let id: Int32
    init(_ id: Int32) { self.id = id }
    deinit { released += 1 }
}
do {
    let h = Handle(1)
}
let result = released
// END PROBE F17.P2

print(result)
