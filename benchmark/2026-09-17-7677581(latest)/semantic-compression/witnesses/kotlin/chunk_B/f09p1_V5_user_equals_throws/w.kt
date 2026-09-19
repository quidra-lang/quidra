// F09.P1 kotlin WITNESS V5 (A4 = user-defined type; A6 = may throw an UNDECLARED exception).
// `equals` is an open member of kotlin.Any, the operand declarations are given context
// OUTSIDE the measured fragment (03 Rule 1.1.2), and Kotlin has no checked exceptions, so
// nothing at the span signals that the comparison can complete abruptly.
class Boom(val v: Int) {
    override fun equals(other: Any?): Boolean = throw IllegalStateException("equals threw")
    override fun hashCode(): Int = v
}
fun probe(): Boolean {
    val s1 = Boom(1)
    val s2 = Boom(1)
    // BEGIN PROBE F09.P1
    val eq = s1 == s2
    // END PROBE F09.P1
    return eq
}
fun main() {
    try { println("eq=" + probe()) } catch (e: Throwable) { println("THREW " + e.javaClass.name + ": " + e.message) }
}
