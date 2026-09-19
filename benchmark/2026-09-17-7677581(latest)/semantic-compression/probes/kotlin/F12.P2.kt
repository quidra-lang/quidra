fun transform(o: Int?): Int {
    // BEGIN PROBE F12.P2
    val h = { v: Int -> v + 1 }
    val p: Int? = o?.let(h)
    return p ?: 0
    // END PROBE F12.P2
}

fun main() {
    println(transform(null))
}
