public class Main {
  static void inner(){ throw new IllegalStateException("boom"); }
  static void outer(){ inner(); }
  public static void main(String[] a){ try { outer(); } catch (RuntimeException e){ System.out.println("X08 caught " + e.getMessage()); } } }
