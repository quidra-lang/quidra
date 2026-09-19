public class Main { public static void main(String[] a) throws Exception {
  final int[] box = new int[1];
  Thread t = new Thread(() -> box[0] = 42); t.start(); t.join();
  System.out.println("X19 " + box[0]); } }
