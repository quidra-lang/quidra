interface HasVal { fun value(): Int }
class C : HasVal { override fun value() = 7 }
fun <T : HasVal> get(t: T): Int = t.value()
fun main() { println("X16 ${get(C())}") }
