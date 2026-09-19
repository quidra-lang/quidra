import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.ValueLayout

fun main() {
    val linker = Linker.nativeLinker()
    val cos = linker.downcallHandle(
        linker.defaultLookup().find("cos").orElseThrow(),
        FunctionDescriptor.of(ValueLayout.JAVA_DOUBLE, ValueLayout.JAVA_DOUBLE)
    )
    val v = cos.invoke(1.0) as Double
    println("cos(1.0)=%.10f".format(v))
}
