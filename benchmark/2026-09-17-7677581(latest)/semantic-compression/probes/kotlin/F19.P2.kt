fun probe(): Long {
    // BEGIN PROBE F19.P2
    val counter = java.util.concurrent.atomic.AtomicLong(0)
    val t1 = kotlin.concurrent.thread { repeat(1000) { counter.incrementAndGet() } }
    val t2 = kotlin.concurrent.thread { repeat(1000) { counter.incrementAndGet() } }
    t1.join()
    t2.join()
    return counter.get()
    // END PROBE F19.P2
}

fun main() {
    println(probe())
}
