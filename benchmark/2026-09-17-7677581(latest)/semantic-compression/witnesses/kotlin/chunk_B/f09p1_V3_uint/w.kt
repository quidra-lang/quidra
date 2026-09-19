// F09.P1 kotlin WITNESS V3 (A4 = fixed-width unsigned integer). Fragment byte-identical.
fun probe(): Boolean {
    val s1 = 4294967295u
    val s2 = 4294967295u
    // BEGIN PROBE F09.P1
    val eq = s1 == s2
    // END PROBE F09.P1
    return eq
}
fun main() { println("eq=" + probe()) }
