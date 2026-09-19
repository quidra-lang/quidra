import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

class Main {
    static String probe() {
        var xs = new ArrayList<>(List.of(2, 3, 1));
        Integer a = null;
        Integer b = 9;
        // BEGIN PROBE F09.P2
        xs.sort(Comparator.reverseOrder());
        boolean lt = a < b;
        // END PROBE F09.P2
        return xs + " " + lt;
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
