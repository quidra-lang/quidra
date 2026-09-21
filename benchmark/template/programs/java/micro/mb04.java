// MB-04 - Floating-point arithmetic: four accumulators over two arrays.
import java.util.Locale;

class Main {

    // Lehmer / MINSTD generator, frozen for every language in the suite.
    static final class Lcg {
        private long state;

        Lcg(long seed) {
            this.state = seed;
        }

        long nextInt() {
            state = (48271L * state) % 2147483647L;
            return state;
        }

        double nextUnit() {
            return nextInt() / 2147483647.0;
        }
    }

    private static final int M = 4000;
    private static final int R = 75000;
    private static final long SEED = 20264917L;

    /** The four accumulators of one whole workload execution. */
    record Result(double s1, double s2, double s3, double s4) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and regenerates A and B. */
    static Result workload() {
        Lcg gen = new Lcg(SEED);
        double[] a = new double[M];
        double[] b = new double[M];
        for (int i = 0; i < M; i++) {
            a[i] = 0.5 + gen.nextUnit();
        }
        for (int i = 0; i < M; i++) {
            b[i] = 0.5 + gen.nextUnit();
        }

        double s1 = 0.0;
        double s2 = 0.0;
        double s3 = 0.0;
        double s4 = 0.0;
        for (int r = 0; r < R; r++) {
            a[r % M] = a[r % M] + 1.0e-9; // anti-elimination, part of the algorithm
            for (int i = 0; i < M; i++) {
                double av = a[i];
                double bv = b[i];
                s1 = s1 + av * bv;
                s2 = s2 + av / (bv + 2.0);
                s3 = s3 + Math.sqrt(av * av + bv * bv);
                s4 = s4 + (av - bv) * (av - bv);
            }
        }

        return new Result(s1, s2, s3, s4);
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

        System.out.printf(Locale.ROOT, "MB04 s1=%.16e s2=%.16e s3=%.16e s4=%.16e%n",
                res.s1(), res.s2(), res.s3(), res.s4());
    }
}
