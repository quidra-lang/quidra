// F10.P2 kotlin WITNESS V4 (left operand kotlin.String: `+` is String.plus(Any?)).
fun probe(i: String, d: Double): String {
    // BEGIN PROBE F10.P2
    val sum = i + d
    // END PROBE F10.P2
    return sum
}
fun main() { val r = probe("3", 0.5); println("sum=" + r + " cls=" + (r as Any).javaClass.name) }
