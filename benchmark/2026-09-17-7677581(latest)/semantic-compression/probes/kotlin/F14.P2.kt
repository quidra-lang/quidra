sealed interface Shape

class Circle(val r: Double) : Shape

class Rect(val w: Double, val h: Double) : Shape

fun areaOf(s: Shape): Double {
    // BEGIN PROBE F14.P2
    val area: Double = when (s) {
        is Circle -> 3.141592653589793 * s.r * s.r
        is Rect -> s.w * s.h
    }
    return area
    // END PROBE F14.P2
}

fun main() {
    println(areaOf(Circle(2.0)))
}
