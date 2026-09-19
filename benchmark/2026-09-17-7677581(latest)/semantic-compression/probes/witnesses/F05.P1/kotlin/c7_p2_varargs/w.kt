fun f(vararg v: MutableList<Int>) {
    println("varargs: callee got a fresh array of length " + v.size)
    v[0][0] = 99
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
}
