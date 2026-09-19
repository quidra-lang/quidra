fun main() {
    val f1 = java.util.concurrent.CompletableFuture.supplyAsync { Thread.currentThread().name }
    val f2 = java.util.concurrent.CompletableFuture.supplyAsync { Thread.currentThread().name }
    println(Thread.currentThread().name + " | " + f1.join() + " | " + f2.join())
}
