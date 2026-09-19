fun main() {
    println("ADV-START")
    System.out.flush()
    val a: Double = readln().trim().toDouble()
    val b: Double = readln().trim().toDouble()
    val h: Double = a / b
    val mean: Double = (1.0 + h + 3.0) / 3.0
    val diff: Double = h - h
    println("OBS=MEAN:" + String.format("%.6f", mean) + "|DIFF:" + String.format("%.6f", diff))
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
