import java.util.List;

class Main {
    static final class Box {
        int id;

        Box(int id) {
            this.id = id;
        }
    }

    static boolean probe() {
        // BEGIN PROBE F04.P3
        Box first = new Box(5);
        Box second = List.of(first).get(0);
        boolean same = first == second;
        return same;
        // END PROBE F04.P3
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
