import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        long a = Long.parseLong(sc.nextLine());
        long b = Long.parseLong(sc.nextLine());
        long q = a / b;
        System.out.println("OBS=QUOT:" + q);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
