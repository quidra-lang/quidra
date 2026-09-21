class A(val v: Long)
class B(val v: Long)

fun main() {
    println("ADV-START")
    System.out.flush()
    val a: Any = A(42L)
    val b: B = a as B
    println("OBS=V:${b.v}")
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
