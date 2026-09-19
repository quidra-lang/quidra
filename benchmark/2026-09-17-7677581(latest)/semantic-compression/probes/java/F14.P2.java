class Main {
    sealed interface Shape {}

    record Circle(double r) implements Shape {}

    record Rect(double w, double h) implements Shape {}

    static double areaOf(Shape s) {
        // BEGIN PROBE F14.P2
        double area = switch (s) {
            case Circle(double r) -> 3.141592653589793 * r * r;
            case Rect(double w, double h) -> w * h;
        };
        return area;
        // END PROBE F14.P2
    }

    public static void main(String[] args) {
        System.out.println(areaOf(new Circle(2.0)));
    }
}
