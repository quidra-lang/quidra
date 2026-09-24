import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;

/** MB-10 — file I/O: buffered text write, read-back, integer formatting and parsing. */
class Main {

    private static final int N = 1_000_000;
    private static final int R = 3;
    private static final int SEED = 20270917;

    /** Section 4.12(c): frozen buffer size, identical for every configuration. */
    private static final int BUF = 65536;

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
    record Result(long sumV, long chk, long nbytes, long lines) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and rewrites the files. */
    static Result workload() throws IOException {
        Lcg g = new Lcg(SEED);
        long sumV = 0;
        long chk = 0;
        long nbytes = 0;
        long lines = 0;

        for (int r = 0; r < R; r++) {
            String name = "mb10_round_" + r + ".txt";

            try (BufferedWriter out = new BufferedWriter(new FileWriter(name), BUF)) {
                for (int i = 0; i < N; i++) {
                    long v = g.nextInt();
                    String line = i + " " + v + "\n";
                    out.write(line);
                    nbytes += line.length();
                }
            }

            try (BufferedReader in = new BufferedReader(new FileReader(name), BUF)) {
                long idx = 0;
                String line;
                while ((line = in.readLine()) != null) {
                    int space = line.indexOf(' ');
                    long a = Long.parseLong(line.substring(0, space));
                    long v = Long.parseLong(line.substring(space + 1));
                    if (a != idx) {
                        throw new IllegalStateException("index mismatch at line " + idx);
                    }
                    idx++;
                    lines++;
                    sumV = (sumV + v) % 1_000_000_007L;
                    chk = (chk * 31 + (v % 1000003)) % 1000003;
                }
            }
        }

        return new Result(sumV, chk, nbytes, lines);
    }

    public static void main(String[] args) throws IOException {
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

        System.out.println("MB10 " + res.sumV() + " " + res.chk() + " " + res.nbytes()
                + " " + res.lines());
    }
}
