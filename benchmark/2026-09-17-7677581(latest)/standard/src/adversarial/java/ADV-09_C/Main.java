public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        long[] xs = { 10L, 20L, 30L, 40L, 50L };
        long e = xs[-1];
        System.out.println("OBS=ELEM:" + e);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
