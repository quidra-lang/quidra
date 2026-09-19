import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** MB-11 — collections: standard hash map, hash set and growable array under load. */
class Main {

    private static final int N = 1_000_000;
    private static final int R = 3;
    private static final long KM = 500009L;
    private static final long SM = 100003L;
    private static final long Q = 1_000_000_007L;
    private static final int SEED = 20271917;

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

    /** The reported fields of one whole workload execution. */
    record Result(long acc, long size1, long found, long vsum, long mchk, long size2,
                  long size3, long lsum) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and regenerates keys. */
    static Result workload() {
        Lcg g = new Lcg(SEED);
        long[] keys = new long[N];
        for (int i = 0; i < N; i++) {
            keys[i] = g.nextInt();
        }

        long acc = 0;
        long size1 = 0;
        long found = 0;
        long vsum = 0;
        long mchk = 0;
        long size2 = 0;
        long size3 = 0;
        long lsum = 0;

        for (int r = 0; r < R; r++) {
            keys[r] = keys[r] + 1000000;

            Map<Long, Long> m = new HashMap<>();
            for (int i = 0; i < N; i++) {
                long k = keys[i] % KM;
                m.put(k, m.getOrDefault(k, 0L) + 1);
            }
            size1 = m.size();

            found = 0;
            vsum = 0;
            for (int i = 0; i < N; i++) {
                long k = (keys[i] + 7) % KM;
                Long v = m.get(k);
                if (v != null) {
                    found++;
                    vsum += v;
                }
            }

            mchk = 0;
            for (Map.Entry<Long, Long> e : m.entrySet()) {
                mchk = (mchk + (e.getKey() % 1000003) * e.getValue()) % 1000003;
            }

            for (int i = 0; i < N; i += 2) {
                long k = keys[i] % KM;
                m.remove(k);
            }
            size2 = m.size();

            Set<Long> st = new HashSet<>();
            for (int i = 0; i < N; i++) {
                st.add(keys[i] % SM);
            }
            size3 = st.size();

            List<Long> lst = new ArrayList<>();
            for (int i = 0; i < N; i++) {
                lst.add(keys[i] % 1000);
            }
            lsum = 0;
            for (long v : lst) {
                lsum += v;
            }

            for (long value : new long[] {size1, found, vsum, mchk, size2, size3, lsum}) {
                acc = (acc * 31 + value) % Q;
            }
        }

        return new Result(acc, size1, found, vsum, mchk, size2, size3, lsum);
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

        System.out.println("MB11 " + res.acc() + " " + res.size1() + " " + res.found() + " "
                + res.vsum() + " " + res.mchk() + " " + res.size2() + " " + res.size3()
                + " " + res.lsum());
    }
}
