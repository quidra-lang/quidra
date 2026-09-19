fun probe(): Boolean {
    val s1 = StringBuilder("ab").append("c").toString()
    val s2 = StringBuilder("ab").append("c").toString()
    // BEGIN PROBE F09.P1
    val eq = s1 == s2
    // END PROBE F09.P1
    return eq
}

fun main() {
    println(probe())
}
