class Main {
    public static void main(String[] args) {
        var first = java.util.concurrent.CompletableFuture.supplyAsync(() -> { throw new IllegalStateException("task failed"); });
        try {
            first.join();
        } catch (RuntimeException e) {
            System.out.println("join threw " + e.getClass().getName() + ": " + e.getCause());
        }
    }
}
