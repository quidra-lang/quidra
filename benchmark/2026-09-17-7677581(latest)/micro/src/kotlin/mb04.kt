// MB-04 - Floating-point arithmetic: four accumulators over two arrays.

import java.util.Locale
import kotlin.math.sqrt

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }

    fun nextUnit(): Double = nextInt() / 2147483647.0
}

const val SEED = 20264917L
const val M = 4000
const val R = 75000

class Mb04Result(val s1: Double, val s2: Double, val s3: Double, val s4: Double)

fun workload(): Mb04Result {
    // Section 5.2: the body re-seeds the generator and regenerates its input,
    // so every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)
    val a = DoubleArray(M)
    val b = DoubleArray(M)
    for (i in 0 until M) {
        a[i] = 0.5 + gen.nextUnit()
    }
    for (i in 0 until M) {
        b[i] = 0.5 + gen.nextUnit()
    }

    var s1 = 0.0
    var s2 = 0.0
    var s3 = 0.0
    var s4 = 0.0
    for (r in 0 until R) {
        a[r % M] = a[r % M] + 1.0e-9 // anti-elimination, part of the frozen algorithm
        for (i in 0 until M) {
            val av = a[i]
            val bv = b[i]
            s1 = s1 + av * bv
            s2 = s2 + av / (bv + 2.0)
            s3 = s3 + sqrt(av * av + bv * bv)
            s4 = s4 + (av - bv) * (av - bv)
        }
    }

    return Mb04Result(s1, s2, s3, s4)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb04Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb04Result(0.0, 0.0, 0.0, 0.0)
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
    // Locale.ROOT: section 2.4 fixes '.' as the decimal separator, so the default
    // locale must not be allowed to emit a comma.
    println(
        "MB04 s1=%.16e s2=%.16e s3=%.16e s4=%.16e"
            .format(Locale.ROOT, res.s1, res.s2, res.s3, res.s4)
    )
}
