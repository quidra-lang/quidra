fun main() {
    println("ADV-START")
    System.out.flush()
    val a: Double = readln().trim().toDouble()
    val b: Double = readln().trim().toDouble()
    val m: Double = a / b
    val xs: DoubleArray = doubleArrayOf(3.0, m, 1.0)
    var best: Double = xs[0]
    for (i in 1 until xs.size) {
        val x: Double = xs[i]
        if (x > best) best = x
    }
    val selfeq: Boolean = m == m
    println("OBS=MAX:" + String.format("%.6f", best) + "|SELFEQ:" + selfeq)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
