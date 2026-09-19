import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        double a = Double.parseDouble(sc.nextLine());
        double b = Double.parseDouble(sc.nextLine());
        double h = a / b;
        double mean = (1.0 + h + 3.0) / 3.0;
        double diff = h - h;
        System.out.println("OBS=MEAN:" + String.format("%.6f", mean) + "|DIFF:" + String.format("%.6f", diff));
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
