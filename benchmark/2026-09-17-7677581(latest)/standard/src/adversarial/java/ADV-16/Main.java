public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        final long x = 10;
        x = 20;
        System.out.println("OBS=V:" + x);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
