var saved: MutableList<Int> = mutableListOf()

fun f(v: MutableList<Int>) {
    v.add(4)
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
    println("resized to " + saved.size + " and retained")
}
