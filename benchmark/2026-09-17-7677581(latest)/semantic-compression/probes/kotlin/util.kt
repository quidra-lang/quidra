// BEGIN PROBE F18.P2
package util

fun pubAdd(a: Int, b: Int): Int = a + b + secret()

private fun secret(): Int = 1
// END PROBE F18.P2
