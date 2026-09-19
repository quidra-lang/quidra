extension Array {
    subscript(i: Int) -> Element { self[self.count - 1 - i] }
}
protocol Named { func tag() -> String }
struct A: Named { func tag() -> String { "a" } }
struct B: Named { func tag() -> String { "b" } }
let items: [Named] = [A(), B()]
let firstTag = items[0].tag()

print(firstTag)
