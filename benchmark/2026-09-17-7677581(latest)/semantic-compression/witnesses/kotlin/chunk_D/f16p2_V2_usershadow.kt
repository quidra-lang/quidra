class UserMap {
    val entries: List<UserEntry> = listOf(UserEntry(9))
    operator fun get(k: String): Int? = 7
}
class UserEntry(val value: Int)

infix fun String.to(v: Int): Pair<String, Int> = Pair(this, v)
fun mapOf(vararg pairs: Pair<String, Int>): UserMap = UserMap()

fun probe(): Int {
    // BEGIN PROBE F16.P2
    val mp = mapOf("a" to 1)
    val total = mp.entries.sumOf { it.value }
    val miss = mp["b"] ?: 0
    return total + miss
    // END PROBE F16.P2
}

fun main() { println(probe()) }
