import java.util.Scanner;

public class Main {
    static long pick(boolean b) {
        if (b) {
            return 1;
        }
    }

    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        boolean b = Integer.parseInt(sc.nextLine()) != 0;
        long r = pick(b) * 2;
        System.out.println("OBS=R:" + r);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
