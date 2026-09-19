import java.nio.file.*;
public class Main { public static void main(String[] a) throws Exception {
  Files.writeString(Path.of("x18.txt"), "hello");
  String d = Files.readString(Path.of("x18.txt"));
  System.out.println("X18 " + d + " " + Files.exists(Path.of("x18.txt"))); } }
