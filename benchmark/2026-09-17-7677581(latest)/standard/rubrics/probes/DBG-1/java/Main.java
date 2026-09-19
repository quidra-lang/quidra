// DBG-1
import java.util.ArrayList;

class Item {
    int count;
    String name;

    Item(int count, String name) {
        this.count = count;
        this.name = name;
    }
}

public class Main {
    static final long ITERATIONS = 300000000L;
    static final long MODULUS = 1000000007L;

    static long accumulate(ArrayList<Item> items, long iterations) {
        long total = 0;
        for (long i = 0; i < iterations; i++) {
            for (Item item : items) {
                total = (total * 31 + item.count + item.name.length()) % MODULUS;
            }
        }
        return total;
    }

    public static void main(String[] args) {
        ArrayList<Item> items = new ArrayList<Item>();
        items.add(new Item(7, "alpha"));
        items.add(new Item(11, "bravo"));
        items.add(new Item(13, "charlie"));
        long checksum = accumulate(items, ITERATIONS);
        System.out.println("checksum=" + checksum);
    }
}
