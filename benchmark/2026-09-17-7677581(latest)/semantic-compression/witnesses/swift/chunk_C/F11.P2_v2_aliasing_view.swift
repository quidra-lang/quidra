final class View {
    var store: [Int]
    init(_ s: [Int]) { store = s }
    subscript(r: Range<Int>) -> View { View(store) }
    var first: Int? { 777 }
}
func second(_ xs: View) -> Int {
let part = xs[1..<4]
return part.first!
}
print(second(View([10, 20, 30, 40, 50])))
