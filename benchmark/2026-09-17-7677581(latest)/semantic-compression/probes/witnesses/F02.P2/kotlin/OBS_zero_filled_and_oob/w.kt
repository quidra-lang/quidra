fun main() {
    val xs = mutableListOf(7, 8, 9)
    try { xs[5] = 42 } catch (e: Exception) { println("oob: " + e) }
    val buf = UByteArray(16)
    try { println(buf[16]) } catch (e: Exception) { println("oob2: " + e) }
    println("zero-filled: " + buf.joinToString(",") { it.toString() })
}
