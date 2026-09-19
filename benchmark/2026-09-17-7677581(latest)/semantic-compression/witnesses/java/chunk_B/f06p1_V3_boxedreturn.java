import java.util.ArrayList;
import java.util.List;

class Main {
    static Object caller() {
        // BEGIN PROBE F06.P1
        var xs = new ArrayList<>(List.of(1, 2, 3));
        int y = mid(xs);
        return y;
    }

    static int mid(List<Integer> s) { return s.get(1); }
        // END PROBE F06.P1

    public static void main(String[] args) {
        Object o = caller();
        System.out.println(o.getClass().getName() + " " + o);
    }
}
