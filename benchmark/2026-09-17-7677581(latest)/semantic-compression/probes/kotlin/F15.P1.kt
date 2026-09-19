fun combine(): String {
// BEGIN PROBE F15.P1
fun <T> head(values: List<T>): T = values[0]

val a = head(listOf(4, 5, 6))
val b = head(listOf("p", "q"))
return "$a$b"
// END PROBE F15.P1
}

fun main() {
    println(combine())
}
