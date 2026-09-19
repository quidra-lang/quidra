// F10.P2 kotlin WITNESS V5 (arbitrary precision: kotlin-stdlib declares operator plus on BigInteger).
import java.math.BigInteger
fun probe(i: BigInteger, d: BigInteger): BigInteger {
    // BEGIN PROBE F10.P2
    val sum = i + d
    // END PROBE F10.P2
    return sum
}
fun main() { println("sum=" + probe(BigInteger("9223372036854775807"), BigInteger.ONE) + " (exact, no overflow edge)") }
