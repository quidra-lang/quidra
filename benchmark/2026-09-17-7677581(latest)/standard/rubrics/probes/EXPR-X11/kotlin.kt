const val V: Int = 1 * 2 * 3 * 4 * 5     // compile-time constant
@Retention(AnnotationRetention.RUNTIME) annotation class Tag(val n: Int)
@Tag(V) class Marked                       // only a compile-time constant is admissible here
fun main() { println("X11 ${Marked::class.java.getAnnotation(Tag::class.java).n}") }
