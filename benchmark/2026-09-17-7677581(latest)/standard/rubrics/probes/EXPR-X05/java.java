import java.util.function.IntUnaryOperator;
public class Main { static IntUnaryOperator makeAdder(int n){ return x -> x + n; }
  public static void main(String[] a){ System.out.println("X05 " + makeAdder(10).applyAsInt(5)); } }
