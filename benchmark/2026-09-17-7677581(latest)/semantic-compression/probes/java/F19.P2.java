class Main {
    static long sharedTotal() throws InterruptedException {
        // BEGIN PROBE F19.P2
        var counter = new java.util.concurrent.atomic.AtomicLong(0);
        Runnable bump = () -> { for (int i = 0; i < 1000; i++) counter.incrementAndGet(); };
        var t1 = Thread.startVirtualThread(bump);
        var t2 = Thread.startVirtualThread(bump);
        t1.join();
        t2.join();
        return counter.get();
        // END PROBE F19.P2
    }

    public static void main(String[] args) throws InterruptedException {
        System.out.println(sharedTotal());
    }
}
