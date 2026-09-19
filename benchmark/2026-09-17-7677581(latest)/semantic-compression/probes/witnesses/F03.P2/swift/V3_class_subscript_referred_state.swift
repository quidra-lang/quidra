final class Buf { var a = [7, 8, 9]; subscript(i: Int) -> Int { get { a[i] } set { a[i] = newValue } } }
func probe() -> String {
    let other = Buf()
    var xs = other
xs[1] = 42
    return "visibleThroughOtherHandle=\(other[1]) sameObject=\(xs === other)"
}
print("s3 " + probe())
