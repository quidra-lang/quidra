fun probe(): Int {
    val xs = mutableListOf(4, 5, 6)
    // BEGIN PROBE F04.P2
    val window: List<Int> = xs
    return window[0]
    // END PROBE F04.P2
}

fun main() {
    println(probe())
}
