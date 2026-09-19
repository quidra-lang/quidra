import shapes.Shape

fun triArea(): Double {
// BEGIN PROBE F14.P3
class Tri(val b: Double, val h: Double) : Shape {
    override fun area(): Double = 0.5 * b * h
}

val s: Shape = Tri(3.0, 4.0)
return s.area()
// END PROBE F14.P3
}

fun main() {
    println(triArea())
}
