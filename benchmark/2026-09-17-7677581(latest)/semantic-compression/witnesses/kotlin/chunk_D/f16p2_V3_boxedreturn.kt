fun probe(): Any {
    // BEGIN PROBE F16.P2
    val mp = mapOf("a" to 1)
    val total = mp.entries.sumOf { it.value }
    val miss = mp["b"] ?: 0
    return total + miss
    // END PROBE F16.P2
}
fun main() { val r = probe(); println(r.javaClass.toString() + " " + r) }
