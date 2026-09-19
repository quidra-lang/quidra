// BEGIN PROBE F15.P2
fun <T : Comparable<T>> maxOf(a: T, b: T): T = if (a > b) a else b

fun probe(): Int {
    val m = maxOf(3, 5)
    return m
}
// END PROBE F15.P2

fun main() {
    println(probe())
}
