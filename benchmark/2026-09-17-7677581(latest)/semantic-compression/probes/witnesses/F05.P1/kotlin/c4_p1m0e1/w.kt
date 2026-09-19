var saved: MutableList<Int> = mutableListOf()

fun f(v: MutableList<Int>) {
    saved = v
}

fun probe(): Int {
    // BEGIN PROBE F05.P1
    val x = mutableListOf(1, 2, 3)
    f(x)
    return x[0]
    // END PROBE F05.P1
}

fun main() {
    println("result=" + probe())
    saved[0] = 77
    println("retained, post-return write visible through the retained alias: " + saved[0])
}
