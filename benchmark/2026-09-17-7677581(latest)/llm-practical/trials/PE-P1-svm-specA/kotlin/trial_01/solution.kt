// WL-SVM — soft-margin SVM by dual gradient ascent (Gauss-Seidel), spec A.
//
// Three logical modules, per spec 2.1:
//   Data — the LCG-PM generator and the dataset builder
//   Svm  — training, support-vector extraction, w/b recovery, f, g, evaluation
//   App  — entry point, output formatting
//
// Standard library only.  No linear-algebra / tensor / statistics / BLAS facility
// is used anywhere: every accumulation below is an explicit scalar loop in the
// pinned index order (frozen rule C-9).  Single threaded.

import java.lang.StringBuilder
import java.util.Locale

// ---------------------------------------------------------------- constants

const val D = 4
const val N_TRAIN_C1 = 100
const val N_TRAIN_C2 = 100
const val N_TEST_C1 = 50
const val N_TEST_C2 = 50
val MU1 = doubleArrayOf(1.0, 1.0, 0.5, -0.5)
val MU2 = doubleArrayOf(-1.0, -1.0, -0.5, 0.5)
const val SIGMA = 0.8
const val SEED = 1234567L
const val C = 10.0
const val LR = 0.0001
const val LIMIT = 0.0001
const val SWEEPS = 1000
const val EPS_SV = 0.0000001

const val N = N_TRAIN_C1 + N_TRAIN_C2   // 200

// ------------------------------------------------------------ module: Data

object Data {

    /** Lehmer / Park-Miller minimal-standard multiplicative generator. */
    class LcgPm(seed: Long) {
        private var state: Long = seed

        fun nextState(): Long {
            state = (MULT * state) % MOD
            return state
        }

        fun nextUniform(): Double = nextState().toDouble() / 2147483647.0

        /** Irwin-Hall(12) - 6.  Ascending accumulation, never reassociated. */
        fun nextNormal(): Double {
            var t = 0.0
            for (k in 0 until 12) {
                t += nextUniform()
            }
            return t - 6.0
        }

        companion object {
            const val MULT = 48271L
            const val MOD = 2147483647L
        }
    }

    /** One block: `count` points, dimensions ascending, drawn from `rng`. */
    fun drawBlock(rng: LcgPm, mu: DoubleArray, count: Int): Array<DoubleArray> {
        val out = Array(count) { DoubleArray(D) }
        for (n in 0 until count) {
            for (d in 0 until D) {
                out[n][d] = mu[d] + SIGMA * rng.nextNormal()
            }
        }
        return out
    }

    class Dataset(
        val xTrain: Array<DoubleArray>,
        val yTrain: DoubleArray,
        val xTestC1: Array<DoubleArray>,
        val xTestC2: Array<DoubleArray>
    )

    /**
     * Exactly one stream, seeded once, drawn strictly sequentially, never reset:
     * train +1, train -1, test +1, test -1.
     */
    fun build(): Dataset {
        val rng = LcgPm(SEED)
        val trainC1 = drawBlock(rng, MU1, N_TRAIN_C1)
        val trainC2 = drawBlock(rng, MU2, N_TRAIN_C2)
        val testC1 = drawBlock(rng, MU1, N_TEST_C1)
        val testC2 = drawBlock(rng, MU2, N_TEST_C2)

        val x = Array(N) { DoubleArray(D) }
        val y = DoubleArray(N)
        for (i in 0 until N_TRAIN_C1) {
            for (d in 0 until D) x[i][d] = trainC1[i][d]
            y[i] = 1.0
        }
        for (i in 0 until N_TRAIN_C2) {
            for (d in 0 until D) x[N_TRAIN_C1 + i][d] = trainC2[i][d]
            y[N_TRAIN_C1 + i] = -1.0
        }
        return Dataset(x, y, testC1, testC2)
    }
}

// ------------------------------------------------------------- module: Svm

object Svm {

    class Model(
        val alpha: DoubleArray,
        val beta: Double,
        val judge: Boolean,
        val errorLast: Double,
        val maxAbsDelta: Double,
        val sMargin: IntArray,
        val sInside: IntArray,
        val w: DoubleArray,
        val b: Double,
        val gram: Array<DoubleArray>
    )

    /** G[i][j] = sum over d ascending of x[i][d]*x[j][d].  Precomputed once (mandatory). */
    fun gram(x: Array<DoubleArray>): Array<DoubleArray> {
        val g = Array(N) { DoubleArray(N) }
        for (i in 0 until N) {
            val xi = x[i]
            for (j in 0 until N) {
                val xj = x[j]
                var s = 0.0
                for (d in 0 until D) {
                    s += xi[d] * xj[d]
                }
                g[i][j] = s
            }
        }
        return g
    }

    fun train(x: Array<DoubleArray>, y: DoubleArray): Model {
        val g = gram(x)
        val alpha = DoubleArray(N)   // all 0.0
        var beta = 1.0

        var judge = false
        var error = 0.0
        var maxAbsDelta = 0.0

        for (sweep in 0 until SWEEPS) {
            judge = false
            error = 0.0
            maxAbsDelta = 0.0

            // (3.1) update alpha, ascending i, in place (Gauss-Seidel)
            for (i in 0 until N) {
                val gi = g[i]
                val yi = y[i]

                var item1 = 0.0
                for (j in 0 until N) {
                    item1 += alpha[j] * yi * y[j] * gi[j]
                }

                var item2 = 0.0
                for (j in 0 until N) {
                    item2 += alpha[j] * yi * y[j]
                }

                val delta = 1.0 - item1 - beta * item2

                val ad = Math.abs(delta)
                if (ad > maxAbsDelta) maxAbsDelta = ad

                alpha[i] = alpha[i] + LR * delta
                if (alpha[i] < 0.0) {
                    alpha[i] = 0.0
                } else if (alpha[i] > C) {
                    alpha[i] = C
                } else if (ad > LIMIT) {
                    judge = true
                    error = error + (ad - LIMIT)
                }
            }

            // (3.2) update beta, once per sweep
            var s = 0.0
            for (i in 0 until N) {
                s += alpha[i] * y[i]
            }
            beta = beta + s * s / 2.0
        }

        // --- support vector extraction
        val marginList = ArrayList<Int>()
        val insideList = ArrayList<Int>()
        for (i in 0 until N) {
            if (EPS_SV < alpha[i] && alpha[i] < C - EPS_SV) marginList.add(i)
            if (alpha[i] >= C - EPS_SV) insideList.add(i)
        }
        val sMargin = IntArray(marginList.size) { marginList[it] }
        val sInside = IntArray(insideList.size) { insideList[it] }

        // --- w recovery
        val w = DoubleArray(D)
        for (d in 0 until D) {
            var acc = 0.0
            for (k in sMargin.indices) {
                val i = sMargin[k]
                acc += alpha[i] * y[i] * x[i][d]
            }
            for (k in sInside.indices) {
                val i = sInside[k]
                acc += alpha[i] * y[i] * x[i][d]
            }
            w[d] = acc
        }

        // --- b recovery
        var bAcc = 0.0
        for (k in sMargin.indices) {
            val i = sMargin[k]
            var dp = 0.0
            for (d in 0 until D) {
                dp += w[d] * x[i][d]
            }
            bAcc += y[i] - dp
        }
        val b = bAcc / sMargin.size.toDouble()

        return Model(alpha, beta, judge, error, maxAbsDelta, sMargin, sInside, w, b, g)
    }

    fun f(w: DoubleArray, b: Double, p: DoubleArray): Double {
        var s = 0.0
        for (d in 0 until D) {
            s += w[d] * p[d]
        }
        return s + b
    }

    fun g(w: DoubleArray, b: Double, p: DoubleArray): Int =
        if (f(w, b, p) >= 0.0) 1 else -1

    /** OBJECTIVE, per spec 2.5. */
    fun objective(alpha: DoubleArray, y: DoubleArray, gm: Array<DoubleArray>): Double {
        var sumA = 0.0
        for (i in 0 until N) sumA += alpha[i]

        var quad = 0.0
        for (i in 0 until N) {
            val gi = gm[i]
            var inner = 0.0
            for (j in 0 until N) {
                inner += alpha[i] * alpha[j] * y[i] * y[j] * gi[j]
            }
            quad += inner
        }
        return sumA - 0.5 * quad
    }

    fun alphaSum(alpha: DoubleArray): Double {
        var s = 0.0
        for (i in 0 until N) s += alpha[i]
        return s
    }

    fun alphaYSum(alpha: DoubleArray, y: DoubleArray): Double {
        var s = 0.0
        for (i in 0 until N) s += alpha[i] * y[i]
        return s
    }

    fun alphaChecksum(alpha: DoubleArray): Double {
        var s = 0.0
        for (i in 0 until N) s += alpha[i] * ((i % 97) + 1).toDouble()
        return s
    }
}

// ------------------------------------------------------------- module: App

object App {

    private fun norm(v: Double): Double = if (v == 0.0) 0.0 else v

    fun f6(v: Double): String = String.format(Locale.ROOT, "%.6f", norm(v))

    fun f12(v: Double): String = String.format(Locale.ROOT, "%.12f", norm(v))

    fun run(): String {
        val ds = Data.build()
        val m = Svm.train(ds.xTrain, ds.yTrain)

        val converged = if (m.judge) 0 else 1

        // --- training accuracy over the 200 training points, index order
        var trainCorrect = 0
        for (i in 0 until N) {
            val pred = Svm.g(m.w, m.b, ds.xTrain[i])
            val truth = if (ds.yTrain[i] >= 0.0) 1 else -1
            if (pred == truth) trainCorrect++
        }
        val trainAcc = trainCorrect.toDouble() / N.toDouble()

        // --- test predictions: class +1 block then class -1 block
        val pred = IntArray(N_TEST_C1 + N_TEST_C2)
        var correctC1 = 0
        for (i in 0 until N_TEST_C1) {
            val p = Svm.g(m.w, m.b, ds.xTestC1[i])
            pred[i] = p
            if (p == 1) correctC1++
        }
        var correctC2 = 0
        for (i in 0 until N_TEST_C2) {
            val p = Svm.g(m.w, m.b, ds.xTestC2[i])
            pred[N_TEST_C1 + i] = p
            if (p == -1) correctC2++
        }
        val totalCorrect = correctC1 + correctC2
        val testAcc = totalCorrect.toDouble() / (N_TEST_C1 + N_TEST_C2).toDouble()
        val testAccC1 = correctC1.toDouble() / N_TEST_C1.toDouble()
        val testAccC2 = correctC2.toDouble() / N_TEST_C2.toDouble()

        val sb = StringBuilder()
        sb.append("SVM_VERSION 1\n")
        sb.append("SWEEPS ").append(SWEEPS).append('\n')
        sb.append("CONVERGED ").append(converged).append('\n')
        sb.append("ERROR_LAST ").append(f6(m.errorLast)).append('\n')
        sb.append("MAX_ABS_DELTA ").append(f6(m.maxAbsDelta)).append('\n')
        sb.append("BETA ").append(f6(m.beta)).append('\n')
        sb.append("NS_MARGIN ").append(m.sMargin.size).append('\n')
        sb.append("NS_INSIDE ").append(m.sInside.size).append('\n')

        sb.append("W")
        for (d in 0 until D) sb.append(' ').append(f6(m.w[d]))
        sb.append('\n')

        sb.append("B ").append(f6(m.b)).append('\n')
        sb.append("OBJECTIVE ").append(f6(Svm.objective(m.alpha, ds.yTrain, m.gram))).append('\n')
        sb.append("ALPHA_SUM ").append(f6(Svm.alphaSum(m.alpha))).append('\n')
        sb.append("ALPHA_Y_SUM ").append(f12(Svm.alphaYSum(m.alpha, ds.yTrain))).append('\n')
        sb.append("ALPHA_CHECKSUM ").append(f6(Svm.alphaChecksum(m.alpha))).append('\n')
        sb.append("TRAIN_ACC ").append(f6(trainAcc)).append('\n')
        sb.append("TEST_ACC ").append(f6(testAcc)).append('\n')
        sb.append("TEST_ACC_C1 ").append(f6(testAccC1)).append('\n')
        sb.append("TEST_ACC_C2 ").append(f6(testAccC2)).append('\n')
        sb.append("TEST_CORRECT ").append(correctC1).append(' ')
            .append(correctC2).append(' ').append(totalCorrect).append('\n')

        sb.append("PRED")
        for (k in pred.indices) sb.append(' ').append(pred[k])
        sb.append('\n')

        sb.append("ALPHA")
        for (i in 0 until N) sb.append(' ').append(f6(m.alpha[i]))
        sb.append('\n')

        return sb.toString()
    }
}

fun main() {
    val out = java.io.PrintStream(java.io.FileOutputStream(java.io.FileDescriptor.out), false, "US-ASCII")
    out.print(App.run())
    out.flush()
}
