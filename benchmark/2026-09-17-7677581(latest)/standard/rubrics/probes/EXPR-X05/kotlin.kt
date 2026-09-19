fun makeAdder(n: Int): (Int) -> Int = { x -> x + n }
fun main() { println("X05 ${makeAdder(10)(5)}") }
