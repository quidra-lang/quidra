class Main {
    // BEGIN PROBE F14.P1
    sealed interface Shape {}

    record Circle(double r) implements Shape {}

    record Rect(double w, double h) implements Shape {}

    static Shape newShape() {
        Shape s = new Circle(2.0);
        return s;
    }
    // END PROBE F14.P1

    public static void main(String[] args) {
        System.out.println(newShape());
    }
}
