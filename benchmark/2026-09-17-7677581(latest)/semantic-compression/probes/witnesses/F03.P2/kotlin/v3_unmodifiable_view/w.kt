fun probe(): Int {
    val xs: MutableList<Int> = java.util.Collections.unmodifiableList(mutableListOf(7, 8, 9))
    // BEGIN PROBE F03.P2
    xs[1] = 42
    // END PROBE F03.P2
    return xs[1]
}
fun main() {
    try { println(probe()) } catch (e: Throwable) { println("unmodifiable view: " + e) }
}
