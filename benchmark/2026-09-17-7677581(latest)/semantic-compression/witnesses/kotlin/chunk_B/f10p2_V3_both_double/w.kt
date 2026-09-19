// F10.P2 kotlin WITNESS V3 (both operands binary floating point).
fun probe(i: Double, d: Double): Double {
    // BEGIN PROBE F10.P2
    val sum = i + d
    // END PROBE F10.P2
    return sum
}
fun main() { println("sum=" + probe(3.0, 0.5) + " inf=" + probe(Double.MAX_VALUE, Double.MAX_VALUE)) }
