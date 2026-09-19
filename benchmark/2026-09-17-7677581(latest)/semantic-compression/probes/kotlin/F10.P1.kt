fun probe(big: Long): Int {
    // BEGIN PROBE F10.P1
    val small = big.toInt()
    return small
    // END PROBE F10.P1
}

fun main() {
    println(probe(2147483648L))
}
