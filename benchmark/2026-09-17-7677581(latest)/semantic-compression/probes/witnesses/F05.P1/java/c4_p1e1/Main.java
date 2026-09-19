import java.util.ArrayList;
import java.util.List;

class Main {
    static List<Integer> kept;

    static void f(List<Integer> v) {
        kept = v;
    }

    static int probe() {
        // BEGIN PROBE F05.P1
        var x = new ArrayList<>(List.of(1, 2, 3));
        f(x);
        return x.get(0);
        // END PROBE F05.P1
    }

    public static void main(String[] args) {
        int before = probe();
        kept.set(0, 77);
        System.out.println("before=" + before + " after-retained-write kept.get(0)=" + kept.get(0));
    }
}
