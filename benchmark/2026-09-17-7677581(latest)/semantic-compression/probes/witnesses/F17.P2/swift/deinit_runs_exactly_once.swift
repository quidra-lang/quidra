var released: Int32 = 0

class Handle {
    let id: Int32
    init(_ id: Int32) { self.id = id }
    deinit { released += 1; print("deinit for id", id, "released now", released) }
}
do {
    let h = Handle(1)
    print("inside block, released =", released)
}
print("after block, released =", released)
