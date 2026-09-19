// MB-07 - Sorting: bottom-up iterative merge sort, ascending, stable, ping-pong buffers.

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }
}

const val SEED = 20267917L
const val N = 2_000_000
const val R = 4

/** Sorts [a] ascending using [buf] as the alternate half of the ping-pong pair. */
fun msort(a: LongArray, buf: LongArray, n: Int) {
    var src = a
    var dst = buf
    var width = 1
    while (width < n) {
        var lo = 0
        while (lo < n) {
            val mid = minOf(lo + width, n)
            val hi = minOf(lo + 2 * width, n)
            var i = lo
            var j = mid
            var k = lo
            while (i < mid && j < hi) {
                if (src[i] <= src[j]) {
                    dst[k] = src[i]
                    i++
                } else {
                    dst[k] = src[j]
                    j++
                }
                k++
            }
            while (i < mid) {
                dst[k] = src[i]
                i++
                k++
            }
            while (j < hi) {
                dst[k] = src[j]
                j++
                k++
            }
            lo += 2 * width
        }
        val tmp = src
        src = dst
        dst = tmp
        width *= 2
    }
    if (src !== a) {
        src.copyInto(a, 0, 0, n)
    }
}

class Mb07Result(val total: Long, val ssum: Long, val inv: Long)

fun workload(): Mb07Result {
    // Section 5.2: the body re-seeds the generator and regenerates its input,
    // so every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)
    val src = LongArray(N)
    for (i in 0 until N) {
        src[i] = gen.nextInt()
    }
    val buf = LongArray(N)

    var total = 0L
    var ssum = 0L
    var inv = 0L
    for (r in 0 until R) {
        src[r] = src[r] + 1 // anti-elimination, part of the frozen algorithm
        val a = src.copyOf()
        msort(a, buf, N)

        var chk = 0L
        for (i in 0 until N) {
            chk = (chk * 31 + (a[i] % 1000003)) % 1000003
        }
        total = (total * 7 + chk) % 1000003

        ssum = 0L
        for (i in 0 until N) {
            ssum += a[i]
        }
        for (i in 1 until N) {
            if (a[i - 1] > a[i]) {
                inv++
            }
        }
    }

    return Mb07Result(total, ssum, inv)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb07Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb07Result(0L, 0L, 0L)
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
    println("MB07 ${res.total} ${res.ssum} ${res.inv}")
}
