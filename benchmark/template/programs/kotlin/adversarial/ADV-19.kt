fun f(n: Long): Long {
    return 1 + f(n + 1)
}

fun main() {
    println("ADV-START")
    System.out.flush()
    val start: Long = readln().toLong()
    val r = f(start)
    println("OBS=R:" + r)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
