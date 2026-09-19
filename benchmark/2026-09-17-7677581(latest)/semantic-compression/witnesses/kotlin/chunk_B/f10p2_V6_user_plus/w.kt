// F10.P2 kotlin WITNESS V6 (a user type declaring `operator fun plus`).
class Money(val cents: Int) {
    var adds = 0
    operator fun plus(other: Money): Money { adds++; if (other.cents < 0) throw IllegalArgumentException("negative"); return Money(cents + other.cents) }
    override fun toString() = "Money(" + cents + ")"
}
fun probe(i: Money, d: Money): Money {
    // BEGIN PROBE F10.P2
    val sum = i + d
    // END PROBE F10.P2
    return sum
}
fun main() {
    val a = Money(300); println("sum=" + probe(a, Money(50)) + " adds=" + a.adds)
    try { probe(a, Money(-1)) } catch (t: Throwable) { println("THREW " + t.javaClass.name) }
}
