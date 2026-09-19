// CONC-1, Java. Worker mechanism: java.lang.Thread (platform threads, java.base).
public class Main {
    static final long N = 500000000L;
    static final int CHUNKS = 4;
    static final long SPAN = N / CHUNKS;
    static final double[] partial = new double[CHUNKS];

    static double chunkSum(int c) {
        double s = 0.0;
        long start = (long) c * SPAN;
        long end = start + SPAN;
        for (long i = start; i < end; i++) { double x = Math.sin((double) i); s += x * x; }
        return s;
    }

    public static void main(String[] args) throws InterruptedException {
        final int W = args.length > 0 ? Integer.parseInt(args[0]) : 1;
        Thread[] th = new Thread[W];
        for (int w = 0; w < W; w++) {
            final int ww = w;
            th[w] = new Thread(() -> {
                for (int c = 0; c < CHUNKS; c++) if (c % W == ww) partial[c] = chunkSum(c);
            });
            th[w].start();
        }
        for (Thread t : th) t.join();
        double total = 0.0;
        for (int c = 0; c < CHUNKS; c++) total = total + partial[c];
        System.out.printf("workers=%d result=%.10f%n", W, total);
    }
}
