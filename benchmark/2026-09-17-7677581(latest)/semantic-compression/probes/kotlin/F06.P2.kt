fun caller(): Int {
    // BEGIN PROBE F06.P2
    fun divmod2(a: Int, b: Int) = a / b to a % b
    val (q, r) = divmod2(7, 3)
    return q + r
    // END PROBE F06.P2
}

fun main() {
    println(caller())
}
