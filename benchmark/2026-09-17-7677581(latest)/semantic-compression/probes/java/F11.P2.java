import java.util.ArrayList;
import java.util.List;

class Main {
    static int subRange() {
        var xs = new ArrayList<>(List.of(10, 20, 30, 40, 50));
        // BEGIN PROBE F11.P2
        var part = xs.subList(1, 4);
        return part.get(0);
        // END PROBE F11.P2
    }

    public static void main(String[] args) {
        System.out.println(subRange());
    }
}
