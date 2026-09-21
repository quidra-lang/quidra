import java.util.Arrays;
import java.util.Locale;

/** MB-09 — statistics: two-pass moments, Pearson correlation, histogram. */
class Main {

    private static final int N = 2_000_000;
    private static final int R = 30;
    private static final int SEED = 20269917;

    /** Park-Miller (Lehmer / MINSTD) generator: state = (state * 48271) mod 2147483647. */
    private static final class Lcg {
        private long state;

        Lcg(long seed) {
            this.state = seed;
        }

        long nextInt() {
            state = (state * 48271L) % 2147483647L;
            return state;
        }

        double nextUnit() {
            return nextInt() / 2147483647.0;
        }
    }

    /** The reported fields of one whole workload execution. */
    record Result(double mean, double var, double sd, double mn, double mx, double mad,
                  double pearson, long histChk) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and regenerates X and Y. */
    static Result workload() {
        Lcg g = new Lcg(SEED);
        double[] x = new double[N];
        double[] y = new double[N];
        for (int i = 0; i < N; i++) {
            x[i] = g.nextUnit() * 100.0;
        }
        for (int i = 0; i < N; i++) {
            y[i] = g.nextUnit() * 100.0;
        }

        double mean = 0.0;
        double var = 0.0;
        double sd = 0.0;
        double mad = 0.0;
        double mn = 0.0;
        double mx = 0.0;
        double pearson = 0.0;
        long histChk = 0;
        int[] hist = new int[64];

        for (int r = 0; r < R; r++) {
            x[r] = x[r] + 1.0e-9;

            double s = 0.0;
            mn = x[0];
            mx = x[0];
            for (int i = 0; i < N; i++) {
                double v = x[i];
                s = s + v;
                if (v < mn) {
                    mn = v;
                }
                if (v > mx) {
                    mx = v;
                }
            }
            mean = s / N;

            double sq = 0.0;
            double ad = 0.0;
            for (int i = 0; i < N; i++) {
                double d = x[i] - mean;
                sq = sq + d * d;
                ad = ad + (d < 0 ? -d : d);
            }
            var = sq / N;
            sd = Math.sqrt(var);
            mad = ad / N;

            double sy = 0.0;
            for (int i = 0; i < N; i++) {
                sy = sy + y[i];
            }
            double meany = sy / N;

            double sxy = 0.0;
            double sxx = 0.0;
            double syy = 0.0;
            for (int i = 0; i < N; i++) {
                double dx = x[i] - mean;
                double dy = y[i] - meany;
                sxy = sxy + dx * dy;
                sxx = sxx + dx * dx;
                syy = syy + dy * dy;
            }
            pearson = sxy / Math.sqrt(sxx * syy);

            Arrays.fill(hist, 0);
            for (int i = 0; i < N; i++) {
                int b = (int) Math.floor(x[i] * 0.64);
                if (b < 0) {
                    b = 0;
                }
                if (b > 63) {
                    b = 63;
                }
                hist[b]++;
            }
            histChk = 0;
            for (int b = 0; b < 64; b++) {
                histChk += (long) (b + 1) * hist[b];
            }
        }

        return new Result(mean, var, sd, mn, mx, mad, pearson, histChk);
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

        System.out.printf(
                Locale.ROOT,
                "MB09 mean=%.16e var=%.16e sd=%.16e min=%.16e max=%.16e mad=%.16e pearson=%.16e"
                        + " hist_chk=%d%n",
                res.mean(), res.var(), res.sd(), res.mn(), res.mx(), res.mad(), res.pearson(),
                res.histChk());
    }
}
