final class View {
    var store: [Int]
    init(_ s: [Int]) { store = s }
    subscript(i: Int) -> Int { get { store[i] } set { store[i] = newValue } }
}
struct Holder {
    let backing = View([7, 8, 9])
    subscript(r: UnboundedRange) -> View { backing }
}
func probe() -> String {
    let xs = Holder()
let window = xs[...]
    let e = window[0]
    window[0] = 99
    return "e0=\(e) afterWrite=\(xs.backing[0]) writableThroughWindow=true"
}
print(probe())
