public class Main { record Point(int x, int y) {}
  public static void main(String[] a){ Point p = new Point(3,4); System.out.println("X01 " + p.x() + " " + p.y()); } }
