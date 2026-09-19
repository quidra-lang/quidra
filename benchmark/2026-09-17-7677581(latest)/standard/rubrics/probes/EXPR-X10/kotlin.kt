data class V(val x: Int, val y: Int) { operator fun plus(o: V) = V(x + o.x, y + o.y) }
fun main() { val v = V(1, 2) + V(3, 4); println("X10 ${v.x} ${v.y}") }
