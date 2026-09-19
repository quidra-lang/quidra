fun fallback(): Int {
    // BEGIN PROBE F12.P1
    val o: Int? = null
    val n: Int = o ?: 0
    return n
    // END PROBE F12.P1
}

fun main() {
    println(fallback())
}
