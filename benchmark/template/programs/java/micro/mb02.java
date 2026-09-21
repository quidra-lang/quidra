// MB-02 - Factorial: recompute k! mod M from scratch for every k.
class Main {

    private static final long M = 1000003L;
    private static final int N = 20000;

    /** The whole workload body (methodology 06 section 5.2). */
    static long workload() {
        long total = 0;
        for (int k = 1; k <= N; k++) {
            long f = 1;
            for (int j = 2; j <= k; j++) {
                f = (f * j) % M;
            }
            total = (total + f) % M;
        }
        return total;
    }

    public static void main(String[] args) {
        String mode = args.length > 0 ? args[0] : "once";
        int k = args.length > 1 ? Integer.parseInt(args[1]) : 7;

        long total;
        if (mode.equals("steady")) {
            total = 0;
            for (int i = 0; i < k; i++) {
                long t0 = System.nanoTime(); // section 5.2 monotonic clock for Java
                total = workload();
                long elapsed = System.nanoTime() - t0;
                System.out.println("ITER " + i + " " + elapsed);
                System.out.flush(); // section 5.2: flushed immediately
            }
        } else {
            total = workload();
        }

        System.out.println("MB02 " + total);
    }
}
