fun probe(a: Int, b: Int): String {
    // BEGIN PROBE F07.P2
    val q = a / b
    val m = a % b
    val d = a.toDouble() / b
    // END PROBE F07.P2
    return "$q $m $d"
}

fun main() {
    println(probe(-7, 2))
}
