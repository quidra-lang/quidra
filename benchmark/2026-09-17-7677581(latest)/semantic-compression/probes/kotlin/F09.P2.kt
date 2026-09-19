fun probe(): String {
    val xs = mutableListOf(2, 3, 1)
    val a = 4
    val b = 9
    // BEGIN PROBE F09.P2
    xs.sortDescending()
    val lt = a < b
    // END PROBE F09.P2
    return "$xs $lt"
}

fun main() {
    println(probe())
}
