// F13.P2 kotlin WITNESS V3 (A9 = MAY NOT RETURN).
class Src { fun toIntOrNull(): Int? { while (true) { } } }
fun handled(s: Src): Int {
    // BEGIN PROBE F13.P2
    val n: Int = s.toIntOrNull() ?: 0
    return n
    // END PROBE F13.P2
}
fun main() { println("start"); println(handled(Src())) }
