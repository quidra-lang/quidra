public class Calculator {

    public static int add(int a, int b) {
        return a + b;
    }

    public static int factorial(int n) {
        int result = 1;
        for (int i = 1; i <= n; i++) {
            result = result * i;
        }
        return result;
    }

    public static void main(String[] args) {
        System.out.println(add(2, 3));
        System.out.println(factorial(5));
    }

}
