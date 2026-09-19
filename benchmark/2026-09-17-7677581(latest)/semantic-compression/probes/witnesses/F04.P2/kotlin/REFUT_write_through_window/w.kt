fun probe(): Int {
    val xs = mutableListOf(4, 5, 6)
    val window: List<Int> = xs
    window[0] = 99
    return window[0]
}
