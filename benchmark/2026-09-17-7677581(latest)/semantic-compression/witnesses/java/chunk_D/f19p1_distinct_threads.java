class Main {
    static String concurrentSum() {
        var first = java.util.concurrent.CompletableFuture.supplyAsync(() -> Thread.currentThread().getName());
        var second = java.util.concurrent.CompletableFuture.supplyAsync(() -> Thread.currentThread().getName());
        return Thread.currentThread().getName() + " | " + first.join() + " | " + second.join();
    }

    public static void main(String[] args) { System.out.println(concurrentSum()); }
}
