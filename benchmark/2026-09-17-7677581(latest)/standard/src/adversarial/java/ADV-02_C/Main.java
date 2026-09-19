public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        long a = 9223372036854775807L;
        long s = a + 3;
        System.out.println("OBS=V:" + s);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
