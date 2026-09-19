class Main {
    static int absViaFfi() throws Throwable {
        // BEGIN PROBE F20.P1
        var linker = java.lang.foreign.Linker.nativeLinker();
        var abs = linker.downcallHandle(linker.defaultLookup().find("abs").orElseThrow(),
                java.lang.foreign.FunctionDescriptor.of(java.lang.foreign.ValueLayout.JAVA_INT,
                        java.lang.foreign.ValueLayout.JAVA_INT));
        int magnitude = (int) abs.invokeExact(-3);
        return magnitude;
        // END PROBE F20.P1
    }

    public static void main(String[] args) throws Throwable {
        System.out.println(absViaFfi());
    }
}
