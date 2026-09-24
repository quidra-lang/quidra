import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        double d = Double.parseDouble(sc.nextLine());
        int v = (int) d;
        System.out.println("OBS=V:" + v);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
