struct String: ExpressibleByStringLiteral, CustomStringConvertible {
    var raw: Swift.String
    init(stringLiteral value: Swift.String) { raw = value + "!" }
    var description: Swift.String { raw }
}
protocol Named { func tag() -> String }
struct A: Named { func tag() -> String { "a" } }
struct B: Named { func tag() -> String { "b" } }
let items: [Named] = [A(), B()]
let firstTag = items[0].tag()

print(firstTag)
