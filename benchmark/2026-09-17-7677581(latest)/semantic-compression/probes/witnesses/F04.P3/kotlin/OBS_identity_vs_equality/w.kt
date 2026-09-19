class Box(var id: Int)
fun main() {
    val a = Box(5)
    val b = listOf(a)[0]
    println("same object: " + (a === b))
    println("distinct equal objects: " + (Box(5) === Box(5)))
    println("structural on distinct: " + (Box(5) == Box(5)))
}
