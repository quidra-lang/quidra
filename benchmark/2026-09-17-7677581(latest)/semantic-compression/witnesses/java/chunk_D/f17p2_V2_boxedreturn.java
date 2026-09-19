class Main {
    static int released = 0;

    static Object cleanupCount() {
        // BEGIN PROBE F17.P2
        record Handle(int id) implements AutoCloseable {
            public void close() {
                released++;
            }
        }
        try (Handle h = new Handle(1)) {
        }
        return released;
        // END PROBE F17.P2
    }

    public static void main(String[] args) {
        Object r = cleanupCount();
        System.out.println(r.getClass().getName() + " " + r);
    }
}
