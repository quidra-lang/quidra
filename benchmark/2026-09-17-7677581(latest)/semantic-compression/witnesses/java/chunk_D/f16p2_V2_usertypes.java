class Map {
    static Map of(String k, int v) { System.out.println("user Map.of"); return new Map(); }
    java.util.List<Entry> entrySet() { return java.util.List.of(new Entry(41)); }
    int getOrDefault(String k, int d) { System.out.println("user getOrDefault"); return 100; }
}

class Entry {
    int v;
    Entry(int v) { this.v = v; }
    int getValue() { return v; }
}

class Main {
    static int mapTotal() {
        // BEGIN PROBE F16.P2
        var mp = Map.of("a", 1);
        int total = 0;
        for (var e : mp.entrySet()) total += e.getValue();
        int miss = mp.getOrDefault("b", 0);
        return total + miss;
        // END PROBE F16.P2
    }

    public static void main(String[] args) { System.out.println(mapTotal()); }
}
