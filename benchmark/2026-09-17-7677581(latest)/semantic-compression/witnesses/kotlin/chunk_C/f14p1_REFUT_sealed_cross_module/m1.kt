sealed interface Shape
class Circle(val r: Double) : Shape
class Rect(val w: Double, val h: Double) : Shape
val s: Shape = Circle(2.0)
