var trace = 0

fun probe(): Int {
    val xs = mutableListOf(1, 2, 3)
    var k = 0
    // BEGIN PROBE F05.P3
    val neg = { a: Int -> k - a }
    val ys = xs.map(neg)
    return ys[0]
    // END PROBE F05.P3
        .also { k = 100; trace = neg(1) }
}

fun main() {
    println("result=" + probe() + "; after writing k=100 through the enclosing frame, neg(1) = " + trace)
}
