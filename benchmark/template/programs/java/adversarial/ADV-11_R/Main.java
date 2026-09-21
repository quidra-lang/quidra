import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        long n = Long.parseLong(sc.nextLine());
        long[] xs = new long[n];
        xs[0] = 1L;
        long first = xs[0];
        System.out.println("OBS=ALLOC:" + xs.length + "|FIRST:" + first);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
