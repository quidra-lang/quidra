// BEGIN PROBE F15.P3
interface Named {
    fun tag(): String
}

class A : Named {
    override fun tag(): String = "a"
}

class B : Named {
    override fun tag(): String = "b"
}

fun firstTag(): String {
    val items: List<Named> = listOf(A(), B())
    return items[0].tag()
}
// END PROBE F15.P3

fun main() {
    println(firstTag())
}
