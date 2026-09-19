class Main {
    interface Comparator<T> {
        static <T> Comparator<T> reverseOrder() { System.out.println("user reverseOrder"); return null; }
    }

    static class Bag {
        void sort(Comparator<Integer> c) { System.out.println("user sort: nothing is reordered"); }
        @Override public String toString() { return "[2, 3, 1]"; }
    }

    static String probe() {
        var xs = new Bag();
        int a = 4;
        int b = 9;
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
