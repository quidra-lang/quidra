import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

class Main {
    static int probe() {
        List<Integer> xs = new ArrayList<>(List.of(4, 5, 6));
        // BEGIN PROBE F04.P2
        List<Integer> window = Collections.unmodifiableList(xs);
        return window.get(0);
        // END PROBE F04.P2
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
