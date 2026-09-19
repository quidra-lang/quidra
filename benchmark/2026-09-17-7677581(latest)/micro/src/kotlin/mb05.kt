// MB-05 - Vector inner product: one accumulator, ascending order, repeated R times.

import java.util.Locale

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }

    fun nextUnit(): Double = nextInt() / 2147483647.0
}

const val SEED = 20265917L
const val N = 2000000
const val R = 400

fun workload(): Double {
    // Section 5.2: the body re-seeds the generator and regenerates X and Y,
    // so every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)
    val x = DoubleArray(N)
    val y = DoubleArray(N)
    for (i in 0 until N) {
        x[i] = 0.5 + gen.nextUnit()
    }
    for (i in 0 until N) {
        y[i] = 0.5 + gen.nextUnit()
    }

    var total = 0.0
    for (r in 0 until R) {
        x[r] = x[r] + 1.0e-9 // anti-elimination; R <= N so the index never wraps
        var d = 0.0
        for (i in 0 until N) {
            d = d + x[i] * y[i]
        }
        total = total + d
    }

    return total
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var total: Double
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        total = 0.0
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
    // Locale.ROOT: section 2.4 fixes '.' as the decimal separator.
    println("MB05 total=%.16e".format(Locale.ROOT, total))
}
