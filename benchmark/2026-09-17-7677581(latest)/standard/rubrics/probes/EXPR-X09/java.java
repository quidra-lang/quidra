public class Main {
  static class Res implements AutoCloseable { public void close(){ System.out.print("X09 cleanup "); } }
  public static void main(String[] a){ try (Res r = new Res()) { throw new RuntimeException("boom"); }
    catch (RuntimeException e){ System.out.println("caught"); } } }
