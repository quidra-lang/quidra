public class Main {
  sealed interface Shape permits Circle, Rect {}
  record Circle(double r) implements Shape {}
  record Rect(double w, double h) implements Shape {}
  static String name(Shape s){ return switch (s) { case Circle c -> "circle"; case Rect r -> "rect"; }; }
  public static void main(String[] a){ System.out.println("X02 " + name(new Circle(1)) + " " + name(new Rect(2,3))); } }
