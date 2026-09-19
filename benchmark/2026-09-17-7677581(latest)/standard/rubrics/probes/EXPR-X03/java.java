public class Main { record Box<T>(T v) { T get(){ return v; } }
  public static void main(String[] a){ System.out.println("X03 " + new Box<Integer>(5).get() + " " + new Box<String>("hi").get()); } }
