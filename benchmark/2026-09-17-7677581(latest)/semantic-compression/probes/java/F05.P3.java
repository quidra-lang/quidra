import java.util.ArrayList;
import java.util.List;
import java.util.function.IntUnaryOperator;

class Main {
    static int probe() {
        List<Integer> xs = new ArrayList<>(List.of(1, 2, 3));
        int k = 0;
        // BEGIN PROBE F05.P3
        IntUnaryOperator neg = a -> k - a;
        int[] ys = xs.stream().mapToInt(Integer::intValue).map(neg).toArray();
        return ys[0];
        // END PROBE F05.P3
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
