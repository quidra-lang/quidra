import java.util.ArrayList;
import java.util.List;

class Main {
    static int probe() {
        List<Integer> xs = new ArrayList<>(List.of(7, 8, 9));
        // BEGIN PROBE F03.P2
        xs.set(1, 42);
        // END PROBE F03.P2
        return xs.get(1);
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
