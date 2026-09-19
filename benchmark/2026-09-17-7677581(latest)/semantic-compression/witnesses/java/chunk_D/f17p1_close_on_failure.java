import java.io.FileInputStream;
import java.io.IOException;

class Main {
    static int readAll() throws IOException {
        try (var in = new FileInputStream("data.txt")) {
            String text = new String(in.readAllBytes());
            return text.getBytes().length;
        }
    }

    public static void main(String[] args) throws Exception {
        // same fragment shape, but the body throws: observe that close() still ran
        var in = new FileInputStream("/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581/semantic-compression/probes/java/data.txt");
        try (in) {
            throw new IllegalStateException("read failed");
        } catch (IllegalStateException e) {
            try {
                in.read();
                System.out.println("STREAM STILL OPEN");
            } catch (IOException closed) {
                System.out.println("CLOSED ON FAILURE PATH: " + closed.getMessage());
            }
        }
    }
}
