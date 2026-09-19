class Main {
    static Object java = new Object();
    static int concurrentSum() {
        var first = java.util.concurrent.CompletableFuture.supplyAsync(() -> 20);
        var second = java.util.concurrent.CompletableFuture.supplyAsync(() -> 22);
        int sum = first.join() + second.join();
        return sum;
    }
    public static void main(String[] a) { System.out.println(concurrentSum()); }
}
