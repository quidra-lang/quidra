import java.util.Arrays;

/** MB-07 — sorting: bottom-up iterative merge sort, ascending, stable, ping-pong buffers. */
class Main {

    private static final int N = 2_000_000;
    private static final int R = 4;
    private static final int SEED = 20267917;

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
    }

    private static void msort(long[] a, long[] buf, int n) {
        long[] src = a;
        long[] dst = buf;
        int width = 1;
        while (width < n) {
            int lo = 0;
            while (lo < n) {
                int mid = Math.min(lo + width, n);
                int hi = Math.min(lo + 2 * width, n);
                int i = lo;
                int j = mid;
                int k = lo;
                while (i < mid && j < hi) {
                    if (src[i] <= src[j]) {
                        dst[k] = src[i];
                        i++;
                    } else {
                        dst[k] = src[j];
                        j++;
                    }
                    k++;
                }
                while (i < mid) {
                    dst[k] = src[i];
                    i++;
                    k++;
                }
                while (j < hi) {
                    dst[k] = src[j];
                    j++;
                    k++;
                }
                lo += 2 * width;
            }
            long[] tmp = src;
            src = dst;
            dst = tmp;
            width *= 2;
        }
        if (src != a) {
            System.arraycopy(src, 0, a, 0, n);
        }
    }

    /** The reported fields of one whole workload execution. */
    record Result(long total, long ssum, long inv) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and regenerates src. */
    static Result workload() {
        Lcg g = new Lcg(SEED);
        long[] src = new long[N];
        for (int i = 0; i < N; i++) {
            src[i] = g.nextInt();
        }
        long[] buf = new long[N];

        long total = 0;
        long ssum = 0;
        long inv = 0;
        for (int r = 0; r < R; r++) {
            src[r] = src[r] + 1;
            long[] a = Arrays.copyOf(src, N);
            msort(a, buf, N);

            long chk = 0;
            for (int i = 0; i < N; i++) {
                chk = (chk * 31 + (a[i] % 1000003)) % 1000003;
            }
            total = (total * 7 + chk) % 1000003;

            ssum = 0;
            for (int i = 0; i < N; i++) {
                ssum += a[i];
            }
            for (int i = 1; i < N; i++) {
                if (a[i - 1] > a[i]) {
                    inv++;
                }
            }
        }

        return new Result(total, ssum, inv);
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

        System.out.println("MB07 " + res.total() + " " + res.ssum() + " " + res.inv());
    }
}
