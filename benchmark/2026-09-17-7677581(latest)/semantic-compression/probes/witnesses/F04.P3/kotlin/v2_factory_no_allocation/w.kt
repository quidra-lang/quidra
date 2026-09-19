val cached = Box(5)

class Box(var id: Int)

fun Box(id: Int, dummy: Unit = Unit): Box = cached

fun probe(): Boolean {
    val first = Box(5, Unit)
    val second = listOf(first)[0]
    val same = first === second
    return same
}

fun main() {
    println("same=" + probe() + "; the object handed back is the pre-existing cached one: " + (probe().let { cached } === cached))
}
