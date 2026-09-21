public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        long x = 9223372036854775807L;
        int v = (int) x;
        System.out.println("OBS=V:" + v);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
