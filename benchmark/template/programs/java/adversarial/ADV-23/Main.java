import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public class Main {
    public static void main(String[] args) throws IOException {
        System.out.println("ADV-START");
        System.out.flush();
        String s = Files.readString(Path.of("inputs/ADV-23.bin"));
        System.out.println("OBS=CP:" + s.length());
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
