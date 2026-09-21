// MB-02 - Factorial: recompute k! mod M from scratch for every k.

const val M = 1000003L
const val N = 20000

fun workload(): Long {
    var total = 0L
    for (k in 1..N) {
        // The inner loop restarts from 1 for every k; f is never carried across k.
        var f = 1L
        for (j in 2..k) {
            f = (f * j) % M
        }
        total = (total + f) % M
    }
    return total
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var total: Long
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        total = 0L
        for (i in 0 until iterations) {
            val t0 = System.nanoTime() // section 5.2 pins System.nanoTime() for Kotlin
            total = workload()
            val elapsed = System.nanoTime() - t0
            println("ITER $i $elapsed")
            System.out.flush() // section 5.2: each ITER line is flushed immediately
        }
    } else {
        total = workload()
    }
    println("MB02 $total")
}
