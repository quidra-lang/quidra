// MB-06 - Matrix multiplication: classical i-j-k order over flat row-major arrays.

import java.util.Locale

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }

    fun nextUnit(): Double = nextInt() / 2147483647.0
}

const val SEED = 20266917L
const val N = 512
const val R = 3

class Mb06Result(val sumC: Double, val cFirst: Double, val cLast: Double)

fun workload(): Mb06Result {
    // Section 5.2: the body re-seeds the generator and regenerates A and B,
    // so every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)
    val a = DoubleArray(N * N)
    val b = DoubleArray(N * N)
    val c = DoubleArray(N * N)
    for (i in 0 until N * N) {
        a[i] = gen.nextUnit()
    }
    for (i in 0 until N * N) {
        b[i] = gen.nextUnit()
    }

    for (r in 0 until R) {
        a[r] = a[r] + 1.0e-9 // anti-elimination, part of the frozen algorithm
        for (i in 0 until N) {
            for (j in 0 until N) {
                var s = 0.0
                for (k in 0 until N) {
                    s = s + a[i * N + k] * b[k * N + j]
                }
                c[i * N + j] = s
            }
        }
    }

    var sumC = 0.0
    for (i in 0 until N * N) {
        sumC = sumC + c[i]
    }

    return Mb06Result(sumC, c[0], c[N * N - 1])
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb06Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb06Result(0.0, 0.0, 0.0)
        for (i in 0 until iterations) {
            val t0 = System.nanoTime() // section 5.2 pins System.nanoTime() for Kotlin
            res = workload()
            val elapsed = System.nanoTime() - t0
            println("ITER $i $elapsed")
            System.out.flush() // section 5.2: each ITER line is flushed immediately
        }
    } else {
        res = workload()
    }
    // Locale.ROOT: section 2.4 fixes '.' as the decimal separator.
    println(
        "MB06 sumC=%.16e c_first=%.16e c_last=%.16e"
            .format(Locale.ROOT, res.sumC, res.cFirst, res.cLast)
    )
}
