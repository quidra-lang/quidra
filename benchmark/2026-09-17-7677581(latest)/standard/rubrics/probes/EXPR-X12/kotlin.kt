class Rec(val a: Int, val b: String)
fun main() { println("X12 meta ${Rec::class.java.declaredFields.size}") }
