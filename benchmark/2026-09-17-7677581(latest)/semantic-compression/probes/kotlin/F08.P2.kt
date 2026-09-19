fun guarded(a: Int, b: Int): Int {
    // BEGIN PROBE F08.P2
    val q = if (b == 0) 0 else a / b
    return q
    // END PROBE F08.P2
}

fun main() {
    println(guarded(7, 0))
}
