struct Sink { subscript(i: Int) -> Int { get { 0 } set { } } }
func probe() -> String {
    var xs = Sink()
xs[1] = 42
    return "noEffect=\(xs[1])"
}
print("s4 " + probe())
