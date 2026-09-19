package shapes;

// BEGIN PROBE F14.P3
public interface Shape {
    double area();
}

record Circle(double r) implements Shape {
    public double area() {
        return 3.141592653589793 * r * r;
    }
}

record Rect(double w, double h) implements Shape {
    public double area() {
        return w * h;
    }
}
// END PROBE F14.P3
