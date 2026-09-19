import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

class Main {
    public static void main(String[] args) {
        List<Integer> xs = new ArrayList<>(List.of(4, 5, 6));
        List<Integer> window = Collections.unmodifiableList(xs);
        try {
            window.set(0, 77);
            System.out.println("WRITE ACCEPTED");
        } catch (UnsupportedOperationException e) {
            System.out.println("rejected at RUN TIME: " + e.getClass().getName());
        }
    }
}
