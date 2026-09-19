import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        String t = sc.nextLine();
        long v = Long.parseLong(t);
        long r = v * 2;
        System.out.println("OBS=R:" + r);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
