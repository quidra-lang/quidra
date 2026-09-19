// F09.P1 kotlin WITNESS V4 (A4 = binary floating point).
// Materially different: with statically-Double operands the compiler emits IEEE 754
// equality, so NaN != NaN and the span can answer false for operands that are the SAME
// object -- an outcome kotlin.String contents-equality cannot produce.
fun probe(): Boolean {
    val s1 = Double.NaN
    val s2 = s1
    // BEGIN PROBE F09.P1
    val eq = s1 == s2
    // END PROBE F09.P1
    return eq
}
fun main() { println("eq=" + probe()) }
