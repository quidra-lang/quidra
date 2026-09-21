// MB-01 - Fibonacci: naive double recursion over the arguments 30..37.

// The pinned naive double recursion: no memoisation, no closed form.
fun fib(n: Int): Long = if (n < 2) n.toLong() else fib(n - 1) + fib(n - 2)

fun workload(): Long {
    var total = 0L
    for (n in 30..37) {
        total += fib(n)
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
    println("MB01 $total")
}
