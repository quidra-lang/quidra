sealed class Shape
data class Circle(val r: Double) : Shape()
data class Rect(val w: Double, val h: Double) : Shape()
fun name(s: Shape): String = when (s) { is Circle -> "circle"; is Rect -> "rect" }
fun main() { println("X02 ${name(Circle(1.0))} ${name(Rect(2.0, 3.0))}") }
