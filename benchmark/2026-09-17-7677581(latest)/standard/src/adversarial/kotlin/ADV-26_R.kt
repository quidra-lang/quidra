fun main() {
    println("ADV-START")
    System.out.flush()
    val a = LongArray(7)
    for (i in 0..6) {
        a[i] = readln().toLong()
    }
    val target: Long = readln().toLong()
    var lo = 0
    var hi = 6
    var result = -1
    while (lo <= hi) {
        val mid = (lo + hi) / 2
        if (a[mid] == target) {
            result = mid
            break
        }
        if (a[mid] < target) {
            lo = mid + 1
        } else {
            hi = mid - 1
        }
    }
    println("OBS=IDX:" + result)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
