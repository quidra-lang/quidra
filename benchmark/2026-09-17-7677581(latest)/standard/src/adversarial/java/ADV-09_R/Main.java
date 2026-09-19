import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        long[] xs = { 10L, 20L, 30L, 40L, 50L };
        int i = Integer.parseInt(sc.nextLine());
        long e = xs[i];
        System.out.println("OBS=ELEM:" + e);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
