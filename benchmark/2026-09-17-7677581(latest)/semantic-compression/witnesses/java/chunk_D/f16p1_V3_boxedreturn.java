import java.util.ArrayList;
import java.util.List;

class Main {
    static Object sumSequence() {
        // BEGIN PROBE F16.P1
        var xs = new ArrayList<>(List.of(1, 2, 3));
        int total = xs.stream().reduce(0, Integer::sum);
        return total;
        // END PROBE F16.P1
    }

    public static void main(String[] args) {
        Object r = sumSequence();
        System.out.println(r.getClass().getName() + " " + r);
    }
}
