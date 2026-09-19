// MB-08 - Strings: text construction plus five character-level passes per round.

/** Lehmer / MINSTD generator, frozen for every language in the suite. */
class Lcg(private var state: Long) {
    fun nextInt(): Long {
        state = (48271L * state) % 2147483647L
        return state
    }
}

const val SEED = 20268917L
const val NW = 200_000
const val R = 20
const val Q = 1_000_000_007L

const val SPACE = ' '.code
const val LOWER_A = 'a'.code
const val LOWER_B = 'b'.code
const val LOWER_Z = 'z'.code
const val LOWER_X = 'x'.code

/** Order-sensitive rolling hash over the first [len] characters of [seq]. */
fun rhash(seq: ByteArray, len: Int): Long {
    var h = 0L
    for (i in 0 until len) {
        h = (h * 131 + seq[i].toInt()) % Q
    }
    return h
}

class Mb08Result(val acc: Long, val len: Int, val cntAb: Long, val cntW: Long)

fun workload(): Mb08Result {
    // Section 5.2: the body re-seeds the generator and rebuilds the text, so
    // every steady iteration performs exactly the work `once` performs.
    val gen = Lcg(SEED)

    val words = ArrayList<String>(NW)
    for (w in 0 until NW) {
        val wordLen = (4 + gen.nextInt() % 13).toInt()
        val sb = StringBuilder(wordLen)
        for (c in 0 until wordLen) {
            sb.append((LOWER_A + gen.nextInt() % 26).toInt().toChar())
        }
        words.add(sb.toString())
    }
    // Section 4.12(b): passes 1-4 run on the pinned mutable working buffer (ByteArray).
    val text = words.joinToString(" ").toByteArray(Charsets.US_ASCII)
    val len = text.size

    var acc = 0L
    var cntAb = 0L
    var cntW = 0L
    for (r in 0 until R) {
        val p = 7 * r + 11 // anti-elimination, part of the frozen algorithm
        val pc = text[p].toInt()
        text[p] = if (pc == SPACE) {
            LOWER_X.toByte()
        } else {
            (LOWER_A + (pc - LOWER_A + 1) % 26).toByte()
        }

        val h1 = rhash(text, len) // pass 1

        val u = ByteArray(len) // pass 2: upper-case
        for (i in 0 until len) {
            val c = text[i].toInt()
            u[i] = if (c >= LOWER_A && c <= LOWER_Z) (c - 32).toByte() else c.toByte()
        }
        val h2 = rhash(u, len)

        val v = ByteArray(len) // pass 3: reverse
        for (i in 0 until len) {
            v[i] = text[len - 1 - i]
        }
        val h3 = rhash(v, len)

        cntAb = 0L // pass 4: naive two-character search
        for (i in 0 until len - 1) {
            if (text[i].toInt() == LOWER_A && text[i + 1].toInt() == LOWER_B) {
                cntAb++
            }
        }

        // Pass 5: word count on the language's own string type. Section 4.12(b)
        // pins `String` and `s[i]` for kotlin; S is constructed fresh from the
        // working buffer inside the timed round and is never a byte view.
        val s = String(text, Charsets.US_ASCII)
        cntW = 1L
        for (i in 0 until len) {
            if (s[i] == ' ') {
                cntW++
            }
        }

        for (value in longArrayOf(h1, h2, h3, cntAb, cntW)) {
            acc = (acc * 31 + value) % Q
        }
    }

    return Mb08Result(acc, len, cntAb, cntW)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb08Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb08Result(0L, 0, 0L, 0L)
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
    println("MB08 ${res.acc} ${res.len} ${res.cntAb} ${res.cntW}")
}
