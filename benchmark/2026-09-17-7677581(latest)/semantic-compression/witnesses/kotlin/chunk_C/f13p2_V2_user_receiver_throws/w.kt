// F13.P2 kotlin WITNESS V2 (A6 = may throw an UNDECLARED exception; A9 = may unwind).
// `s` is given context OUTSIDE the measured fragment (`fun handled(s: ...)` sits before BEGIN PROBE),
// so the RECEIVER TYPE is external under 03 Rule 1.1.2. A receiver type carrying a member
// `toIntOrNull(): Int?` that completes abruptly defeats the elvis fallback entirely.
class Src { fun toIntOrNull(): Int? = throw IllegalStateException("member toIntOrNull threw") }
fun handled(s: Src): Int {
    // BEGIN PROBE F13.P2
    val n: Int = s.toIntOrNull() ?: 0
    return n
    // END PROBE F13.P2
}
fun main() {
    try { println(handled(Src())) } catch (t: Throwable) { println("THREW " + t.javaClass.name + ": " + t.message) }
}
