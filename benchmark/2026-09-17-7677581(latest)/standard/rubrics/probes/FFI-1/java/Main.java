import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;

public class Main {
    public static void main(String[] args) throws Throwable {
        Linker linker = Linker.nativeLinker();
        MethodHandle cos = linker.downcallHandle(
                linker.defaultLookup().find("cos").orElseThrow(),
                FunctionDescriptor.of(ValueLayout.JAVA_DOUBLE, ValueLayout.JAVA_DOUBLE));
        System.out.printf("cos(1.0)=%.10f%n", (double) cos.invokeExact(1.0d));
    }
}
