fun caller(): Int {
    // BEGIN PROBE F06.P1
    fun mid(v: MutableList<Int>) = v[1]
    val xs = mutableListOf(1, 2, 3)
    val y = mid(xs)
    return y
    // END PROBE F06.P1
}

fun main() {
    println(caller())
}
