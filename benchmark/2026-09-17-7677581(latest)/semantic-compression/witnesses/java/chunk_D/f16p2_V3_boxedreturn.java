import java.util.Map;

class Main {
    static Object mapTotal() {
        // BEGIN PROBE F16.P2
        var mp = Map.of("a", 1);
        int total = 0;
        for (var e : mp.entrySet()) total += e.getValue();
        int miss = mp.getOrDefault("b", 0);
        return total + miss;
        // END PROBE F16.P2
    }

    public static void main(String[] args) {
        Object r = mapTotal();
        System.out.println(r.getClass().getName() + " " + r);
    }
}
