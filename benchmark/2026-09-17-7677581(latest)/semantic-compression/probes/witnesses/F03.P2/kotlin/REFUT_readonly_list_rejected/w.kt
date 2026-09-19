fun probe(): Int {
    val xs: List<Int> = listOf(7, 8, 9)
    xs[1] = 42
    return xs[1]
}
