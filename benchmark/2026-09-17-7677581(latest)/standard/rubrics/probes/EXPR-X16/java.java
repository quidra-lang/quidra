public class Main {
  interface HasVal { int val(); }
  record C() implements HasVal { public int val(){ return 7; } }
  static <T extends HasVal> int get(T t){ return t.val(); }
  public static void main(String[] a){ System.out.println("X16 " + get(new C())); } }
