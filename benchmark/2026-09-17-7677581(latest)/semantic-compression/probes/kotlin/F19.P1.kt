fun probe(): Int {
    // BEGIN PROBE F19.P1
    val f1 = java.util.concurrent.CompletableFuture.supplyAsync { 20 }
    val f2 = java.util.concurrent.CompletableFuture.supplyAsync { 22 }
    val sum = f1.join() + f2.join()
    // END PROBE F19.P1
    return sum
}

fun main() {
    println(probe())
}
