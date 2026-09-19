fun subRange(xs: MutableList<Int>): Int {
    // BEGIN PROBE F11.P2
    val part = xs.subList(1, 4)
    return part[0]
    // END PROBE F11.P2
}

fun main() {
    println(subRange(mutableListOf(10, 20, 30, 40, 50)))
}
