// F11.P1 kotlin WITNESS V7 (A6 = CANNOT FAIL).
// `xs` is given context OUTSIDE the measured fragment, so its type is external (03 Rule 1.1.2).
// A user `operator fun get` that is TOTAL for every index removes the failure mode entirely --
// an outcome class kotlin.collections.List.get cannot reach.
class Grid { operator fun get(i: Int): Int = 0 }
val g = Grid()
fun element(xs: Grid, i: Int): Int {
    // BEGIN PROBE F11.P1
    val e = xs[i]
    return e
    // END PROBE F11.P1
}
fun main() { println("e=" + element(g, 1000000) + " (index far out of any range; no exception)") }
