import java.util.ArrayList;
import java.util.List;

public class Main {
    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        List<Long> xs = new ArrayList<>();
        xs.add(1L);
        xs.add(2L);
        xs.add(3L);
        xs.add(4L);
        xs.add(5L);
        long iters = 0;
        for (long v : xs) {
            iters = iters + 1;
            if (v == 2) {
                xs.add(99L);
            }
        }
        System.out.println("OBS=ITERS:" + iters + "|LEN:" + xs.size());
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
