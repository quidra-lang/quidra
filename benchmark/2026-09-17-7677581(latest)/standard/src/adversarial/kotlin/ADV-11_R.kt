fun main() {
    println("ADV-START")
    System.out.flush()
    val n: Long = readln().trim().toLong()
    val xs: LongArray = LongArray(n)
    xs[0] = 1L
    val f: Long = xs[0]
    println("OBS=ALLOC:" + xs.size + "|FIRST:" + f)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
