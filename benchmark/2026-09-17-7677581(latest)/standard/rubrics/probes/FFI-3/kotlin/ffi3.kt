import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.MemoryLayout
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout
import java.lang.invoke.MethodHandles
import java.lang.invoke.MethodType

val RECORD: MemoryLayout = MemoryLayout.structLayout(
    ValueLayout.JAVA_INT.withName("count"),
    MemoryLayout.paddingLayout(4),
    ValueLayout.JAVA_DOUBLE.withName("weight")
)

fun cmpDouble(a: MemorySegment, b: MemorySegment): Int =
    java.lang.Double.compare(
        a.reinterpret(8).get(ValueLayout.JAVA_DOUBLE, 0),
        b.reinterpret(8).get(ValueLayout.JAVA_DOUBLE, 0)
    )

fun cmpRecord(a: MemorySegment, b: MemorySegment): Int =
    Integer.compare(
        a.reinterpret(16).get(ValueLayout.JAVA_INT, 0),
        b.reinterpret(16).get(ValueLayout.JAVA_INT, 0)
    )

fun main() {
    println("sizeof=${RECORD.byteSize()} offset=${RECORD.byteOffset(MemoryLayout.PathElement.groupElement("weight"))}")

    val linker = Linker.nativeLinker()
    val qsort = linker.downcallHandle(
        linker.defaultLookup().find("qsort").orElseThrow(),
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS)
    )
    val cmpDesc = FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS)
    val lk = MethodHandles.lookup()
    val owner = Class.forName("Ffi3Kt")
    val mt = MethodType.methodType(Int::class.javaPrimitiveType, MemorySegment::class.java, MemorySegment::class.java)

    Arena.ofConfined().use { arena ->
        val cmpD = linker.upcallStub(lk.findStatic(owner, "cmpDouble", mt), cmpDesc, arena)
        val cmpR = linker.upcallStub(lk.findStatic(owner, "cmpRecord", mt), cmpDesc, arena)

        // Off-heap C-layout buffer, not a Kotlin collection: qsort reorders it in place.
        val xs = arena.allocate(ValueLayout.JAVA_DOUBLE, 5)
        doubleArrayOf(3.5, 1.25, 4.75, 1.5, 2.25).forEachIndexed { i, v ->
            xs.setAtIndex(ValueLayout.JAVA_DOUBLE, i.toLong(), v)
        }
        qsort.invoke(xs, 5L, 8L, cmpD)
        println((0 until 5).joinToString(" ") { "%.2f".format(xs.getAtIndex(ValueLayout.JAVA_DOUBLE, it.toLong())) })

        val rs = arena.allocate(RECORD, 3)
        val counts = intArrayOf(3, 1, 2)
        val weights = doubleArrayOf(1.5, 4.0, 2.5)
        for (i in 0 until 3) {
            rs.set(ValueLayout.JAVA_INT, i * 16L, counts[i])
            rs.set(ValueLayout.JAVA_DOUBLE, i * 16L + 8L, weights[i])
        }
        qsort.invoke(rs, 3L, 16L, cmpR)
        println((0 until 3).joinToString(" ") {
            "${rs.get(ValueLayout.JAVA_INT, it * 16L)}:" + "%.2f".format(rs.get(ValueLayout.JAVA_DOUBLE, it * 16L + 8L))
        })
    }
}
