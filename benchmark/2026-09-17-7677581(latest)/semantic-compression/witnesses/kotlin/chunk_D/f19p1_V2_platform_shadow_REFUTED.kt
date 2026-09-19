package java.util.concurrent

class CompletableFuture<T> {
    companion object {
        fun <U> supplyAsync(s: () -> U): CompletableFuture<U> = CompletableFuture()
    }
    fun join(): T = throw RuntimeException("user")
}
