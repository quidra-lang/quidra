class Main {
    static int concurrentSum() {
        // BEGIN PROBE F19.P1
        var first = java.util.concurrent.CompletableFuture.supplyAsync(() -> 20);
        var second = java.util.concurrent.CompletableFuture.supplyAsync(() -> 22);
        int sum = first.join() + second.join();
        return sum;
        // END PROBE F19.P1
    }

    public static void main(String[] args) {
        System.out.println(concurrentSum());
    }
}
