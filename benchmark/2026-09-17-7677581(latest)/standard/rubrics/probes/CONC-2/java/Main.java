// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
public class Main {
    static final int ITERS = 200000;
    static long counter = 0;

    public static void main(String[] args) throws InterruptedException {
        Runnable bump = () -> { for (int i = 0; i < ITERS; i++) counter = counter + 1; };
        Thread t1 = new Thread(bump), t2 = new Thread(bump);
        t1.start(); t2.start(); t1.join(); t2.join();
        System.out.printf("counter=%d expected=%d%n", counter, 2L * ITERS);
    }
}
