// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
import kotlin.concurrent.thread

const val ITERS: Int = 200000
var counter: Long = 0

fun main() {
    val t1 = thread(start = true) { for (i in 0 until ITERS) counter = counter + 1 }
    val t2 = thread(start = true) { for (i in 0 until ITERS) counter = counter + 1 }
    t1.join(); t2.join()
    println("counter=$counter expected=${2 * ITERS}")
}
