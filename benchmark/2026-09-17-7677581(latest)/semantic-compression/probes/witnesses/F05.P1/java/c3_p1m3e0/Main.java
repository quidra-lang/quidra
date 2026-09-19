import java.util.ArrayList;
import java.util.List;

class Main {
    static ArrayList<Integer> seen;

    static void f(List<Integer> v) {
        v.clear();
        v.add(55);
    }

    static int probe() {
        // BEGIN PROBE F05.P1
        var x = new ArrayList<>(List.of(1, 2, 3));
        f(x);
        return x.get(0);
        // END PROBE F05.P1
    }

    public static void main(String[] args) {
        System.out.println("x.get(0)=" + probe());
    }
}
