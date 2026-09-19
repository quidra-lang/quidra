// F10.P2 kotlin WITNESS V2 (both operands the same fixed-width integer type).
fun probe(i: Int, d: Int): Int {
    // BEGIN PROBE F10.P2
    val sum = i + d
    // END PROBE F10.P2
    return sum
}
fun main() { println("sum=" + probe(3, 1) + " overflow=" + probe(Int.MAX_VALUE, 1)) }
