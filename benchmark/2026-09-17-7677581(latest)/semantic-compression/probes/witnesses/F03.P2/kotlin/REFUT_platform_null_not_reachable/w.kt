fun main() {
    val m = java.util.HashMap<String, MutableList<Int>>()
    val xs = m["absent"]
    println("static-nullability of xs is a platform type; runtime value = " + (xs == null))
    try {
        xs[1] = 42
    } catch (e: Throwable) {
        println("platform-typed null receiver: " + e)
    }
}
