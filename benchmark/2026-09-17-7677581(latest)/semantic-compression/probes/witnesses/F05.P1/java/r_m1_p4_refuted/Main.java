import java.util.ArrayList;
import java.util.List;

class Main {
    static void f(List<Integer> v) {
        v = new ArrayList<>(List.of(7, 7, 7));
        System.out.println("callee rebound its own parameter to " + v);
    }

    static int probe() {
        // BEGIN PROBE F05.P1
        var x = new ArrayList<>(List.of(1, 2, 3));
        f(x);
        return x.get(0);
        // END PROBE F05.P1
    }

    public static void main(String[] args) {
        System.out.println("caller's x.get(0)=" + probe());
    }
}
