import java.util.ArrayList;
import java.util.List;

class Main {
    static int element(int i) {
        var xs = new ArrayList<>(List.of(1, 2, 3));
        // BEGIN PROBE F11.P1
        int e = xs.get(i);
        return e;
        // END PROBE F11.P1
    }

    public static void main(String[] args) {
        System.out.println(element(2));
    }
}
