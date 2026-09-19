import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** MB-08 — strings: text construction plus five character-level passes per round. */
class Main {

    private static final int NW = 200_000;
    private static final int R = 20;
    private static final long Q = 1_000_000_007L;
    private static final int SEED = 20268917;

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

    /**
     * Order-sensitive rolling hash over the first {@code len} bytes of the
     * section 4.12(b) working buffer (pinned for java: {@code byte[]}).
     */
    private static long rhash(byte[] seq, int len) {
        long h = 0;
        for (int i = 0; i < len; i++) {
            h = (h * 131 + seq[i]) % 1_000_000_007L;
        }
        return h;
    }

    /** The reported fields of one whole workload execution. */
    record Result(long acc, int len, long cntAb, long cntW) { }

    /** The whole workload body (methodology 06 section 5.2): re-seeds and rebuilds the text. */
    static Result workload() {
        Lcg g = new Lcg(SEED);

        List<String> words = new ArrayList<>();
        for (int w = 0; w < NW; w++) {
            int len = (int) (4 + g.nextInt() % 13);
            StringBuilder word = new StringBuilder(len);
            for (int i = 0; i < len; i++) {
                word.append((char) ('a' + g.nextInt() % 26));
            }
            words.add(word.toString());
        }
        // Section 4.12(b): java's passes-1-4 working buffer is byte[].
        byte[] text = String.join(" ", words).getBytes(StandardCharsets.US_ASCII);
        int len = text.length;

        long acc = 0;
        long cntAb = 0;
        long cntW = 0;
        for (int r = 0; r < R; r++) {
            int p = 7 * r + 11;
            if (text[p] == ' ') {
                text[p] = 'x';
            } else {
                text[p] = (byte) ('a' + (text[p] - 'a' + 1) % 26);
            }

            long h1 = rhash(text, len); // pass 1

            byte[] u = new byte[len]; // pass 2: upper-case
            for (int i = 0; i < len; i++) {
                byte c = text[i];
                u[i] = (c >= 97 && c <= 122) ? (byte) (c - 32) : c;
            }
            long h2 = rhash(u, len);

            byte[] v = new byte[len]; // pass 3: reverse
            for (int i = 0; i < len; i++) {
                v[i] = text[len - 1 - i];
            }
            long h3 = rhash(v, len);

            cntAb = 0; // pass 4: naive search
            for (int i = 0; i < len - 1; i++) {
                if (text[i] == 'a' && text[i + 1] == 'b') {
                    cntAb++;
                }
            }

            // Pass 5: word count on the language's own string type, constructed fresh from
            // the working buffer inside the timed round and read with the section 4.12(b)
            // pinned access API for java (String.charAt). Never a byte view of the buffer.
            String s = new String(text, StandardCharsets.US_ASCII);
            cntW = 1;
            for (int i = 0; i < len; i++) {
                if (s.charAt(i) == ' ') {
                    cntW++;
                }
            }

            for (long value : new long[] {h1, h2, h3, cntAb, cntW}) {
                acc = (acc * 31 + value) % Q;
            }
        }

        return new Result(acc, len, cntAb, cntW);
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

        System.out.println("MB08 " + res.acc() + " " + res.len() + " " + res.cntAb()
                + " " + res.cntW());
    }
}
