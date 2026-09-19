class Main {
    static int released = 0;

    record Handle(int id) implements AutoCloseable {
        public void close() { released++; }
    }

    public static void main(String[] args) throws Exception {
        Handle h = new Handle(1);
        h = null;
        System.gc();
        Thread.sleep(200);
        System.gc();
        Thread.sleep(200);
        System.out.println("released after the value became unreachable = " + released);
    }
}
