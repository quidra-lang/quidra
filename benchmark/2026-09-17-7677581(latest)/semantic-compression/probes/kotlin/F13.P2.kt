fun handled(s: String): Int {
    // BEGIN PROBE F13.P2
    val n: Int = s.toIntOrNull() ?: 0
    return n
    // END PROBE F13.P2
}

fun main() {
    println(handled("21"))
}
