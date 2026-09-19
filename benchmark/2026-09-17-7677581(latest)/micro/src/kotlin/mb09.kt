// MB-09 - Statistics: two-pass moments, Pearson correlation, histogram.

import java.util.Locale
import kotlin.math.floor
import kotlin.math.sqrt

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }

    fun nextUnit(): Double = nextInt() / 2147483647.0
}

const val SEED = 20269917L
const val N = 2_000_000
const val R = 30

class Mb09Result(
    val mean: Double,
    val variance: Double,
    val sd: Double,
    val mn: Double,
    val mx: Double,
    val mad: Double,
    val pearson: Double,
    val histChk: Long
)

fun workload(): Mb09Result {
    // Section 5.2: the body re-seeds the generator and regenerates X and Y,
    // so every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)
    val x = DoubleArray(N)
    val y = DoubleArray(N)
    for (i in 0 until N) {
        x[i] = gen.nextUnit() * 100.0
    }
    for (i in 0 until N) {
        y[i] = gen.nextUnit() * 100.0
    }

    var mean = 0.0
    var variance = 0.0
    var sd = 0.0
    var mad = 0.0
    var mn = 0.0
    var mx = 0.0
    var pearson = 0.0
    var histChk = 0L
    val hist = IntArray(64)

    for (r in 0 until R) {
        x[r] = x[r] + 1.0e-9 // anti-elimination, part of the frozen algorithm

        var s = 0.0 // pass 1: sum, min, max
        mn = x[0]
        mx = x[0]
        for (i in 0 until N) {
            val v = x[i]
            s = s + v
            if (v < mn) {
                mn = v
            }
            if (v > mx) {
                mx = v
            }
        }
        mean = s / N

        var sq = 0.0 // pass 2: squared and absolute deviations
        var ad = 0.0
        for (i in 0 until N) {
            val d = x[i] - mean
            sq = sq + d * d
            ad = ad + if (d < 0) -d else d
        }
        variance = sq / N
        sd = sqrt(variance)
        mad = ad / N

        var sy = 0.0 // pass 3: mean of Y
        for (i in 0 until N) {
            sy = sy + y[i]
        }
        val meany = sy / N

        var sxy = 0.0 // pass 4: co-moments
        var sxx = 0.0
        var syy = 0.0
        for (i in 0 until N) {
            val dx = x[i] - mean
            val dy = y[i] - meany
            sxy = sxy + dx * dy
            sxx = sxx + dx * dx
            syy = syy + dy * dy
        }
        pearson = sxy / sqrt(sxx * syy)

        hist.fill(0) // pass 5: 64-bin histogram
        for (i in 0 until N) {
            var b = floor(x[i] * 0.64).toInt()
            if (b < 0) {
                b = 0
            }
            if (b > 63) {
                b = 63
            }
            hist[b]++
        }
        histChk = 0L
        for (b in 0 until 64) {
            histChk += (b + 1).toLong() * hist[b]
        }
    }

    return Mb09Result(mean, variance, sd, mn, mx, mad, pearson, histChk)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb09Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb09Result(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0L)
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
        ("MB09 mean=%.16e var=%.16e sd=%.16e min=%.16e max=%.16e mad=%.16e pearson=%.16e" +
            " hist_chk=%d").format(
            Locale.ROOT,
            res.mean,
            res.variance,
            res.sd,
            res.mn,
            res.mx,
            res.mad,
            res.pearson,
            res.histChk
        )
    )
}
