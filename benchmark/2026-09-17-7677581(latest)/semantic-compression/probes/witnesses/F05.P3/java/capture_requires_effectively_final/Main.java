import java.util.function.IntUnaryOperator;
class Main { public static void main(String[] a){ int k = 0; IntUnaryOperator neg = x -> k - x; k = 1; System.out.println(neg.applyAsInt(1)); } }
