fun element(xs: MutableList<Int>, i: Int): Int {
    // BEGIN PROBE F11.P1
    val e = xs[i]
    return e
    // END PROBE F11.P1
}

fun main() {
    println(element(mutableListOf(1, 2, 3), 2))
}
