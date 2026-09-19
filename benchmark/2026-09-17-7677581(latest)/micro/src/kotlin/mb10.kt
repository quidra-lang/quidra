// MB-10 - File I/O: buffered text write, read-back, integer formatting and parsing.

import java.io.File

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }
}

const val SEED = 20270917L
const val N = 1_000_000
const val R = 3

// Section 4.12(c): a single frozen 65536-byte buffer for every configuration.
const val BUF = 65536

class Mb10Result(val sumV: Long, val chk: Long, val nbytes: Long, val lines: Long)

fun workload(): Mb10Result {
    // Section 5.2: the body re-seeds the generator and rewrites its files, so
    // every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED) // the stream continues across rounds
    var sumV = 0L
    var chk = 0L
    var nbytes = 0L
    var lines = 0L

    for (r in 0 until R) {
        val file = File("mb10_round_$r.txt")

        file.bufferedWriter(bufferSize = BUF).use { out ->
            for (i in 0 until N) {
                val v = gen.nextInt()
                val line = "$i $v\n"
                out.write(line)
                nbytes += line.length
            }
        }

        var idx = 0L
        file.bufferedReader(bufferSize = BUF).use { reader ->
            var line = reader.readLine()
            while (line != null) {
                val space = line.indexOf(' ')
                val a = line.substring(0, space).toLong()
                val v = line.substring(space + 1).toLong()
                check(a == idx) { "index mismatch: expected $idx, read $a" }
                idx++
                lines++
                sumV = (sumV + v) % 1_000_000_007L
                chk = (chk * 31 + (v % 1000003)) % 1000003
                line = reader.readLine()
            }
        }
    }

    return Mb10Result(sumV, chk, nbytes, lines)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb10Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb10Result(0L, 0L, 0L, 0L)
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
    println("MB10 ${res.sumV} ${res.chk} ${res.nbytes} ${res.lines}")
}
