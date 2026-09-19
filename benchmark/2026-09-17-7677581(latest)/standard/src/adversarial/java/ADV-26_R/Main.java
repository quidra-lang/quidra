import java.util.Scanner;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        Scanner sc = new Scanner(System.in);
        long[] a = new long[7];
        for (int i = 0; i < 7; i++) {
            a[i] = Long.parseLong(sc.nextLine());
        }
        long target = Long.parseLong(sc.nextLine());
        int lo = 0;
        int hi = 6;
        int result = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (a[mid] == target) {
                result = mid;
                break;
            } else if (a[mid] < target) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        System.out.println("OBS=IDX:" + result);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
