fun elementChecked(xs: MutableList<Int>, i: Int): Int {
    // BEGIN PROBE F11.P1.checked
    val e = xs.getOrNull(i) ?: 0
    return e
    // END PROBE F11.P1.checked
}

fun main() {
    println(elementChecked(mutableListOf(1, 2, 3), 2))
}
