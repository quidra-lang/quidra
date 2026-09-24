// MB-05 - Vector inner product: one accumulator, ascending order, repeated R times.
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

    private static final int N = 2000000;
    private static final int R = 400;
    private static final long SEED = 20265917L;

    /** The whole workload body (methodology 06 section 5.2): re-seeds and regenerates X and Y. */
    static double workload() {
        Lcg gen = new Lcg(SEED);
        double[] x = new double[N];
        double[] y = new double[N];
        for (int i = 0; i < N; i++) {
            x[i] = 0.5 + gen.nextUnit();
        }
        for (int i = 0; i < N; i++) {
            y[i] = 0.5 + gen.nextUnit();
        }

        double total = 0.0;
        for (int r = 0; r < R; r++) {
            x[r] = x[r] + 1.0e-9; // anti-elimination; R <= N so no wrap
            double d = 0.0;
            for (int i = 0; i < N; i++) {
                d = d + x[i] * y[i];
            }
            total = total + d;
        }

        return total;
    }

    public static void main(String[] args) {
        String mode = args.length > 0 ? args[0] : "once";
        int k = args.length > 1 ? Integer.parseInt(args[1]) : 7;

        double total;
        if (mode.equals("steady")) {
            total = 0.0;
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

        System.out.printf(Locale.ROOT, "MB05 total=%.16e%n", total);
    }
}
