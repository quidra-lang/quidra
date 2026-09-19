struct P: CustomStringConvertible { var x: Int; var description: String { "P(x: \(x))" } }
func readAt(_ xs: [P], _ i: Int) -> P {
let e = xs[i]
return e
}
print(readAt([P(x: 1), P(x: 2), P(x: 3)], 2))
