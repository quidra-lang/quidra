import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

class Main {
    public static void main(String[] args) {
        List<Integer> xs = new ArrayList<>(List.of(4, 5, 6));
        List<Integer> window = Collections.unmodifiableList(xs);
        xs.set(0, 77);
        System.out.println("window.get(0)=" + window.get(0));
    }
}
