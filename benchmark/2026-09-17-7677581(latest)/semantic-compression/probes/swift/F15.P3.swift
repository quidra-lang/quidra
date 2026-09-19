// BEGIN PROBE F15.P3
protocol Named { func tag() -> String }
struct A: Named { func tag() -> String { "a" } }
struct B: Named { func tag() -> String { "b" } }
let items: [Named] = [A(), B()]
let firstTag = items[0].tag()
// END PROBE F15.P3

print(firstTag)
