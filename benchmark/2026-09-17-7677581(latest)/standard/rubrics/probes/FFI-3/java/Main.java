import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.StringJoiner;

public class Main {
    static final StructLayout RECORD = MemoryLayout.structLayout(
            ValueLayout.JAVA_INT.withName("count"),
            MemoryLayout.paddingLayout(4),
            ValueLayout.JAVA_DOUBLE.withName("weight"));

    public static int cmpDouble(MemorySegment a, MemorySegment b) {
        double x = a.reinterpret(8).get(ValueLayout.JAVA_DOUBLE, 0);
        double y = b.reinterpret(8).get(ValueLayout.JAVA_DOUBLE, 0);
        return Double.compare(x, y);
    }

    public static int cmpRecord(MemorySegment a, MemorySegment b) {
        int x = a.reinterpret(16).get(ValueLayout.JAVA_INT, 0);
        int y = b.reinterpret(16).get(ValueLayout.JAVA_INT, 0);
        return Integer.compare(x, y);
    }

    public static void main(String[] args) throws Throwable {
        System.out.println("sizeof=" + RECORD.byteSize() + " offset="
                + RECORD.byteOffset(MemoryLayout.PathElement.groupElement("weight")));

        Linker linker = Linker.nativeLinker();
        MethodHandle qsort = linker.downcallHandle(
                linker.defaultLookup().find("qsort").orElseThrow(),
                FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_LONG,
                        ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
        FunctionDescriptor cmpDesc = FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.ADDRESS);
        MethodHandles.Lookup lk = MethodHandles.lookup();
        MethodType mt = MethodType.methodType(int.class, MemorySegment.class, MemorySegment.class);

        try (Arena arena = Arena.ofConfined()) {
            MemorySegment cmpD = linker.upcallStub(lk.findStatic(Main.class, "cmpDouble", mt), cmpDesc, arena);
            MemorySegment cmpR = linker.upcallStub(lk.findStatic(Main.class, "cmpRecord", mt), cmpDesc, arena);

            // Off-heap C-layout buffer, not a java.util collection: qsort reorders it in place.
            MemorySegment xs = arena.allocate(ValueLayout.JAVA_DOUBLE, 5);
            double[] init = {3.5, 1.25, 4.75, 1.5, 2.25};
            for (int i = 0; i < 5; i++) xs.setAtIndex(ValueLayout.JAVA_DOUBLE, i, init[i]);
            qsort.invokeExact(xs, 5L, 8L, cmpD);
            StringJoiner j1 = new StringJoiner(" ");
            for (int i = 0; i < 5; i++) j1.add(String.format("%.2f", xs.getAtIndex(ValueLayout.JAVA_DOUBLE, i)));
            System.out.println(j1);

            MemorySegment rs = arena.allocate(RECORD, 3);
            int[] counts = {3, 1, 2};
            double[] weights = {1.5, 4.0, 2.5};
            for (int i = 0; i < 3; i++) {
                rs.set(ValueLayout.JAVA_INT, i * 16L, counts[i]);
                rs.set(ValueLayout.JAVA_DOUBLE, i * 16L + 8L, weights[i]);
            }
            qsort.invokeExact(rs, 3L, 16L, cmpR);
            StringJoiner j2 = new StringJoiner(" ");
            for (int i = 0; i < 3; i++)
                j2.add(rs.get(ValueLayout.JAVA_INT, i * 16L) + String.format(":%.2f", rs.get(ValueLayout.JAVA_DOUBLE, i * 16L + 8L)));
            System.out.println(j2);
        }
    }
}
