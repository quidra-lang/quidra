class Main {
    static class List<E> {
        Object[] a;
        List(Object[] a) { this.a = a; }
        @SafeVarargs static <E> List<E> of(E... xs) { System.out.println("user List.of"); return new List<>(xs); }
        @SuppressWarnings("unchecked") E get(int i) { System.out.println("user get"); return (E) a[0]; }
    }
    static class ArrayList<E> extends List<E> {
        ArrayList(List<E> src) { super(src.a); System.out.println("user ArrayList ctor"); }
    }
    static int caller() {
        // BEGIN PROBE F06.P1
        var xs = new ArrayList<>(List.of(1, 2, 3));
        int y = mid(xs);
        return y;
    }

    static int mid(List<Integer> s) { return s.get(1); }
        // END PROBE F06.P1

    public static void main(String[] args) {
        System.out.println(caller());
    }
}
