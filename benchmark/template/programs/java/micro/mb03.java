// MB-03 - Integer arithmetic: a five-operation mix driven by the frozen generator.
class Main {

    private static final int N = 120000000;

    /** The four accumulators of one whole workload execution. */
    record Result(long sAdd, long sXor, long sMul, long sDiv) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds the generator itself. */
    static Result workload() {
        long x = 20263917L; // the seed itself is the initial generator state
        long sAdd = 0;
        long sXor = 0;
        long sMul = 1;
        long sDiv = 0;

        for (int i = 0; i < N; i++) {
            x = (48271L * x) % 2147483647L;
            sAdd = (sAdd + x) % 2147483647L;
            sXor = sXor ^ x;
            sMul = (sMul * 33 + (x % 97)) % 1000003L;
            sDiv = sDiv + x / 1000L;
        }

        return new Result(sAdd, sXor, sMul, sDiv);
    }

    public static void main(String[] args) {
        String mode = args.length > 0 ? args[0] : "once";
        int k = args.length > 1 ? Integer.parseInt(args[1]) : 7;

        Result res;
        if (mode.equals("steady")) {
            res = null;
            for (int i = 0; i < k; i++) {
                long t0 = System.nanoTime(); // section 5.2 monotonic clock for Java
                res = workload();
                long elapsed = System.nanoTime() - t0;
                System.out.println("ITER " + i + " " + elapsed);
                System.out.flush(); // section 5.2: flushed immediately
            }
        } else {
            res = workload();
        }

        System.out.println("MB03 " + res.sAdd() + " " + res.sXor() + " " + res.sMul()
                + " " + res.sDiv());
    }
}
