// MB-11 - Collections: standard hash map, hash set and growable array under load.

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }
}

const val SEED = 20271917L
const val N = 1_000_000
const val R = 3
const val KM = 500009L
const val SM = 100003L
const val Q = 1_000_000_007L

class Mb11Result(
    val acc: Long,
    val size1: Long,
    val found: Long,
    val vsum: Long,
    val mchk: Long,
    val size2: Long,
    val size3: Long,
    val lsum: Long
)

fun workload(): Mb11Result {
    // Section 5.2: the body re-seeds the generator and regenerates `keys`, so
    // every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)
    val keys = LongArray(N)
    for (i in 0 until N) {
        keys[i] = gen.nextInt()
    }

    var acc = 0L
    var size1 = 0L
    var found = 0L
    var vsum = 0L
    var mchk = 0L
    var size2 = 0L
    var size3 = 0L
    var lsum = 0L

    for (r in 0 until R) {
        keys[r] = keys[r] + 1000000 // anti-elimination, part of the frozen algorithm

        val m = HashMap<Long, Long>()
        for (i in 0 until N) {
            val k = keys[i] % KM
            m[k] = (m[k] ?: 0L) + 1
        }
        size1 = m.size.toLong()

        found = 0L
        vsum = 0L
        for (i in 0 until N) {
            val k = (keys[i] + 7) % KM
            val v = m[k]
            if (v != null) {
                found++
                vsum += v
            }
        }

        mchk = 0L
        for ((k, v) in m) {
            mchk = (mchk + (k % 1000003) * v) % 1000003
        }

        for (i in 0 until N step 2) {
            val k = keys[i] % KM
            m.remove(k)
        }
        size2 = m.size.toLong()

        val st = HashSet<Long>()
        for (i in 0 until N) {
            st.add(keys[i] % SM)
        }
        size3 = st.size.toLong()

        val lst = ArrayList<Long>()
        for (i in 0 until N) {
            lst.add(keys[i] % 1000)
        }
        lsum = 0L
        for (v in lst) {
            lsum += v
        }

        for (value in longArrayOf(size1, found, vsum, mchk, size2, size3, lsum)) {
            acc = (acc * 31 + value) % Q
        }
    }

    return Mb11Result(acc, size1, found, vsum, mchk, size2, size3, lsum)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb11Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb11Result(0L, 0L, 0L, 0L, 0L, 0L, 0L, 0L)
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
    println(
        "MB11 ${res.acc} ${res.size1} ${res.found} ${res.vsum} ${res.mchk} " +
            "${res.size2} ${res.size3} ${res.lsum}"
    )
}
