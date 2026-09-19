class Box(var id: Int)

fun probe(): Boolean {
    // BEGIN PROBE F04.P3
    val first = Box(5)
    val second = listOf(first)[0]
    val same = first === second
    return same
    // END PROBE F04.P3
}

fun main() {
    println(probe())
}
