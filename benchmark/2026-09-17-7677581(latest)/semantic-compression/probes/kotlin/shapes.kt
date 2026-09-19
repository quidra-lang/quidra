package shapes

// BEGIN PROBE F14.P3
interface Shape {
    fun area(): Double
}

class Circle(val r: Double) : Shape {
    override fun area(): Double = 3.141592653589793 * r * r
}

class Rect(val w: Double, val h: Double) : Shape {
    override fun area(): Double = w * h
}
// END PROBE F14.P3
