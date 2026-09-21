fun main() {
    println("ADV-START")
    System.out.flush()
    val s: Int = readln().trim().toInt()
    val u: UInt = readln().trim().toUInt()
    val c: Boolean = s < u
    println("OBS=CMP:$c")
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
