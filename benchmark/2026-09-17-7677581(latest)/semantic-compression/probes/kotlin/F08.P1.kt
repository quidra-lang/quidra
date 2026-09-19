fun overflowAtMax(): Int {
    // BEGIN PROBE F08.P1
    val m = Int.MAX_VALUE
    val o = m + 1
    return o
    // END PROBE F08.P1
}

fun main() {
    println(overflowAtMax())
}
