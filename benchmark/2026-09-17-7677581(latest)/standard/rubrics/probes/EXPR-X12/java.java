public class Main { record Rec(int a, String b) {}
  public static void main(String[] x){ System.out.println("X12 meta " + Rec.class.getRecordComponents().length); } }
