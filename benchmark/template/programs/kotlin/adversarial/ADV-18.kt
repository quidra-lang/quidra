fun pick(b: Boolean): Long {
    if (b) {
        return 1
    }
}

fun main() {
    println("ADV-START")
    System.out.flush()
    val b: Boolean = readln().toInt() != 0
    val r = pick(b) * 2
    println("OBS=R:" + r)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
