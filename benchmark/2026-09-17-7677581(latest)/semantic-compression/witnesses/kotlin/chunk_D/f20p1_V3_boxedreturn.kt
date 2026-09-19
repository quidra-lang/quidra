fun probe(): Any {
    // BEGIN PROBE F20.P1
    val linker = java.lang.foreign.Linker.nativeLinker()
    val abs = linker.downcallHandle(linker.defaultLookup().find("abs").orElseThrow(),
        java.lang.foreign.FunctionDescriptor.of(java.lang.foreign.ValueLayout.JAVA_INT,
            java.lang.foreign.ValueLayout.JAVA_INT))
    val magnitude = abs.invokeExact(-3) as Int
    return magnitude
    // END PROBE F20.P1
}
fun main() { val r = probe(); println(r.javaClass.toString() + " " + r) }
