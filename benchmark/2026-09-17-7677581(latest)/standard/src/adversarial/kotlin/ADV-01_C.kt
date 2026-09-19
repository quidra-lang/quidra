fun main() {
    println("ADV-START")
    System.out.flush()
    val a: Long = 9223372036854775807L
    val v: Int = a.toInt()
    println("OBS=V:$v")
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
