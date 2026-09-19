// F09.P1 kotlin WITNESS V2 (A4 = fixed-width signed integer). Fragment byte-identical.
fun probe(): Boolean {
    val s1 = 2147483647
    val s2 = 2147483647
    // BEGIN PROBE F09.P1
    val eq = s1 == s2
    // END PROBE F09.P1
    return eq
}
fun main() { println("eq=" + probe()) }
