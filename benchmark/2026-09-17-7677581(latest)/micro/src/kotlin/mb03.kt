// MB-03 - Integer arithmetic: mixed add / xor / multiply-modulo / divide.

const val N = 120000000

class Mb03Result(val sAdd: Long, val sXor: Long, val sMul: Long, val sDiv: Long)

fun workload(): Mb03Result {
    var x = 20263917L // the seed is the initial generator state
    var sAdd = 0L
    var sXor = 0L
    var sMul = 1L
    var sDiv = 0L

    for (i in 0 until N) {
        x = (48271L * x) % 2147483647L
        sAdd = (sAdd + x) % 2147483647L
        sXor = sXor xor x
        sMul = (sMul * 33 + (x % 97)) % 1000003L
        sDiv = sDiv + x / 1000L
    }

    return Mb03Result(sAdd, sXor, sMul, sDiv)
}

fun main(args: Array<String>) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    val mode = if (args.isNotEmpty()) args[0] else "once"
    var res: Mb03Result
    if (mode == "steady") {
        val k = if (args.size > 1) args[1].toIntOrNull() ?: 7 else 7
        val iterations = if (k < 1) 7 else k
        res = Mb03Result(0L, 0L, 0L, 0L)
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
    println("MB03 ${res.sAdd} ${res.sXor} ${res.sMul} ${res.sDiv}")
}
