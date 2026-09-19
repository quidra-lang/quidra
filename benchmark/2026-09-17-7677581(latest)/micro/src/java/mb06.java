// MB-06 - Matrix multiplication: classical i-j-k product over flat row-major arrays.
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

    private static final int N = 512;
    private static final int R = 3;
    private static final long SEED = 20266917L;

    /** The reported fields of one whole workload execution. */
    record Result(double sumC, double cFirst, double cLast) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and regenerates A and B. */
    static Result workload() {
        Lcg gen = new Lcg(SEED);
        double[] a = new double[N * N];
        double[] b = new double[N * N];
        double[] c = new double[N * N];
        for (int i = 0; i < N * N; i++) {
            a[i] = gen.nextUnit();
        }
        for (int i = 0; i < N * N; i++) {
            b[i] = gen.nextUnit();
        }

        for (int r = 0; r < R; r++) {
            a[r] = a[r] + 1.0e-9; // anti-elimination, part of the algorithm
            for (int i = 0; i < N; i++) {
                for (int j = 0; j < N; j++) {
                    double s = 0.0;
                    for (int k = 0; k < N; k++) {
                        s = s + a[i * N + k] * b[k * N + j];
                    }
                    c[i * N + j] = s;
                }
            }
        }

        double sumC = 0.0;
        for (int i = 0; i < N * N; i++) {
            sumC = sumC + c[i];
        }

        return new Result(sumC, c[0], c[N * N - 1]);
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

        System.out.printf(Locale.ROOT, "MB06 sumC=%.16e c_first=%.16e c_last=%.16e%n",
                res.sumC(), res.cFirst(), res.cLast());
    }
}
