class Main {
    static int caller() {
        // BEGIN PROBE F06.P2
        return switch (divmod2(7, 3)) { case QR(int q, int r) -> q + r; };
    }

    record QR(int q, int r) {}

    static QR divmod2(int a, int b) { return new QR(a / b, a % b); }
        // END PROBE F06.P2

    public static void main(String[] args) {
        System.out.println(caller());
    }
}
