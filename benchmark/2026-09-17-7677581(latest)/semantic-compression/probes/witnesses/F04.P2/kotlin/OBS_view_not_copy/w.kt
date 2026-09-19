fun main() {
    val xs = mutableListOf(4, 5, 6)
    val window: List<Int> = xs
    xs[0] = 77
    println("no copy, window[0] = " + window[0])
    println("identity: " + (window === xs))
    @Suppress("UNCHECKED_CAST")
    (window as MutableList<Int>)[0] = 88
    println("downcast write visible through xs: " + xs[0])
}
