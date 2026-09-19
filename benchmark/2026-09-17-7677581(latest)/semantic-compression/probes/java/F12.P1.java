import java.util.OptionalInt;

class Main {
    static int fallback() {
        // BEGIN PROBE F12.P1
        OptionalInt o = OptionalInt.empty();
        int n = o.orElse(0);
        return n;
        // END PROBE F12.P1
    }

    public static void main(String[] args) {
        System.out.println(fallback());
    }
}
