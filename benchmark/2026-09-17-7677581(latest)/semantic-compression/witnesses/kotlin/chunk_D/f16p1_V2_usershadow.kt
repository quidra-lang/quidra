class UserSeq {
    fun sum(): Int = 0
}

fun mutableListOf(vararg elements: Int): UserSeq = UserSeq()

fun probe(): Int {
    // BEGIN PROBE F16.P1
    val xs = mutableListOf(1, 2, 3)
    val total = xs.sum()
    return total
    // END PROBE F16.P1
}

fun main() {
    println(probe())
}
