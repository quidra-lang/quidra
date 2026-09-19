fun probe(): Int {
    val xs = mutableListOf(1, 2, 3)
    val k = 0
    // BEGIN PROBE F05.P3
    val neg = { a: Int -> k - a }
    val ys = xs.map(neg)
    return ys[0]
    // END PROBE F05.P3
}

fun main() {
    println(probe())
}
