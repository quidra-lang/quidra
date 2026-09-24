// MB-01 - Fibonacci: naive double recursion over the arguments 30..37.
class Main {

    // The pinned naive double recursion: no memoisation, no closed form.
    static long fib(int n) {
        if (n < 2) {
            return n;
        }
        return fib(n - 1) + fib(n - 2);
    }

    /** The whole workload body (methodology 06 section 5.2). */
    static long workload() {
        long total = 0;
        for (int n = 30; n <= 37; n++) {
            total += fib(n);
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

        System.out.println("MB01 " + total);
    }
}
