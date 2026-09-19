import java.io.FileInputStream;
import java.io.IOException;

class Main {
    // BEGIN PROBE F17.P1
    static int readAll() throws IOException {
        try (var in = new FileInputStream("data.txt")) {
            String text = new String(in.readAllBytes());
            return text.getBytes().length;
        }
    }
    // END PROBE F17.P1

    public static void main(String[] args) throws IOException {
        System.out.println(readAll());
    }
}
