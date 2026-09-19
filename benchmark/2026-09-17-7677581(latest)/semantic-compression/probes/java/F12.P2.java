import java.util.OptionalInt;
import java.util.function.IntUnaryOperator;

class Main {
    static int transform() {
        OptionalInt o = OptionalInt.empty();
        // BEGIN PROBE F12.P2
        IntUnaryOperator h = v -> v + 1;
        OptionalInt p = o.stream().map(h).findFirst();
        return p.orElse(0);
        // END PROBE F12.P2
    }

    public static void main(String[] args) {
        System.out.println(transform());
    }
}
