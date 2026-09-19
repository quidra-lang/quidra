fun probe(): UByte {
    // BEGIN PROBE F02.P2
    val buf = UByteArray(16)
    buf[0] = 1u
    return buf[0]
    // END PROBE F02.P2
}

fun main() {
    println(probe())
}
