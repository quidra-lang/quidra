import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        double a = Double.parseDouble(sc.nextLine());
        double b = Double.parseDouble(sc.nextLine());
        double m = a / b;
        double[] xs = { 3.0, m, 1.0 };
        double best = xs[0];
        for (int i = 1; i < xs.length; i++) {
            if (xs[i] > best) {
                best = xs[i];
            }
        }
        boolean selfeq = (m == m);
        System.out.println("OBS=MAX:" + String.format("%.6f", best) + "|SELFEQ:" + selfeq);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
