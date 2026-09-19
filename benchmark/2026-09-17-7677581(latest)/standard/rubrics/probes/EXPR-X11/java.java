public class Main {
  static final int V = 1 * 2 * 3 * 4 * 5;   // JLS 15.29 constant expression
  public static void main(String[] a){
    switch (120) { case V -> System.out.println("X11 " + V); default -> System.out.println("X11 FAIL"); } } }
