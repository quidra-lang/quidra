import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        long a = Long.parseLong(sc.nextLine());
        long s = a + 3;
        System.out.println("OBS=V:" + s);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
