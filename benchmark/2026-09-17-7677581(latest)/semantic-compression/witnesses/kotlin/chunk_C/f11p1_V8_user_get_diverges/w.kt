// F11.P1 kotlin WITNESS V8 (A9 = MAY NOT RETURN).
class Grid { operator fun get(i: Int): Int { while (true) { } } }
val g = Grid()
fun element(xs: Grid, i: Int): Int {
    // BEGIN PROBE F11.P1
    val e = xs[i]
    return e
    // END PROBE F11.P1
}
fun main() { println("start"); println(element(g, 2)) }
