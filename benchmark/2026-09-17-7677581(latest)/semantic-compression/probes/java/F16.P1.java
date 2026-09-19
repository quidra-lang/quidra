import java.util.ArrayList;
import java.util.List;

class Main {
    static int sumSequence() {
        // BEGIN PROBE F16.P1
        var xs = new ArrayList<>(List.of(1, 2, 3));
        int total = xs.stream().reduce(0, Integer::sum);
        return total;
        // END PROBE F16.P1
    }

    public static void main(String[] args) {
        System.out.println(sumSequence());
    }
}
