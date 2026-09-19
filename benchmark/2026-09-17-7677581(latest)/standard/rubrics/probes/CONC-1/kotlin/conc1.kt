// CONC-1, Kotlin/JVM. Worker mechanism: kotlin.concurrent.thread (Kotlin standard library).
import kotlin.concurrent.thread

const val N: Long = 500000000L
const val CHUNKS: Int = 4
const val SPAN: Long = N / CHUNKS

val partial = DoubleArray(CHUNKS)

fun chunkSum(c: Int): Double {
    var s = 0.0
    val start = c.toLong() * SPAN
    val end = start + SPAN
    var i = start
    while (i < end) { val x = Math.sin(i.toDouble()); s += x * x; i++ }
    return s
}

fun main(args: Array<String>) {
    val w = if (args.isNotEmpty()) args[0].toInt() else 1
    val ths = (0 until w).map { wi ->
        thread(start = true) {
            for (c in 0 until CHUNKS) if (c % w == wi) partial[c] = chunkSum(c)
        }
    }
    ths.forEach { it.join() }
    var total = 0.0
    for (c in 0 until CHUNKS) total = total + partial[c]
    println(String.format("workers=%d result=%.10f", w, total))
}
