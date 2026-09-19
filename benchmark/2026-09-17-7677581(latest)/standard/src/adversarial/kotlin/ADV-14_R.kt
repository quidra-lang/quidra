fun scale(n: Long): Long {
    return n * 3
}

fun main() {
    println("ADV-START")
    System.out.flush()
    val t: String = readln()
    val r = scale(t)
    println("OBS=R:" + r)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
