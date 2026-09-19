import shapes.Shape;

class Main {
    static double triArea() {
        // BEGIN PROBE F14.P3
        record Tri(double b, double h) implements Shape {
            public double area() {
                return 0.5 * b * h;
            }
        }
        Shape s = new Tri(3.0, 4.0);
        return s.area();
        // END PROBE F14.P3
    }

    public static void main(String[] args) {
        System.out.println(triArea());
    }
}
