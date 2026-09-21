fun main() {
    println("ADV-START")
    System.out.flush()
    val xs = mutableListOf<Long>(1, 2, 3, 4, 5)
    var iters = 0
    for (x in xs) {
        iters += 1
        if (x == 2L) xs.add(99L)
    }
    println("OBS=ITERS:" + iters + "|LEN:" + xs.size)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
