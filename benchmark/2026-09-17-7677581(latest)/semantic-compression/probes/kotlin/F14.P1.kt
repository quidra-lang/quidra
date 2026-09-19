// BEGIN PROBE F14.P1
sealed interface Shape

class Circle(val r: Double) : Shape

class Rect(val w: Double, val h: Double) : Shape

val s: Shape = Circle(2.0)
// END PROBE F14.P1

fun main() {
    println(s is Circle)
}
