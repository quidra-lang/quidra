import java.util.List;

class Main {
    static final class Box {
        int id;

        Box(int id) {
            this.id = id;
        }
    }

    public static void main(String[] args) {
        Box first = new Box(5);
        Box second = List.of(first).get(0);
        Box equalValue = new Box(5);
        System.out.println("routed==first? " + (first == second)
                + "   distinct-but-field-equal==first? " + (first == equalValue));
    }
}
