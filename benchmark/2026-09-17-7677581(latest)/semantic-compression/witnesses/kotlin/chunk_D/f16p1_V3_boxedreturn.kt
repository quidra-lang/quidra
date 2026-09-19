fun probe(): Any {
    // BEGIN PROBE F16.P1
    val xs = mutableListOf(1, 2, 3)
    val total = xs.sum()
    return total
    // END PROBE F16.P1
}

fun main() {
    val r = probe()
    println(r.javaClass.toString() + " " + r)
}
