import java.util.Scanner;

public class Main {
    static long f(long n) {
        return 1 + f(n + 1);
    }

    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        long n = Long.parseLong(sc.nextLine());
        long r = f(n);
        System.out.println("OBS=R:" + r);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
