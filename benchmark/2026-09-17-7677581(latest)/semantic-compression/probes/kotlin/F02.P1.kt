fun probe(cond: Boolean): Int {
    // BEGIN PROBE F02.P1
    val v: Int
    if (cond) v = 5 else v = 9
    return v
    // END PROBE F02.P1
}

fun main() {
    println(probe(true))
}
