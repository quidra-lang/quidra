class Main {
    public static void main(String[] args) throws Throwable {
        var linker = java.lang.foreign.Linker.nativeLinker();
        var sym = linker.defaultLookup().find("abs").orElseThrow();
        System.out.println("resolved native symbol abs at " + sym);
        var abs = linker.downcallHandle(sym,
                java.lang.foreign.FunctionDescriptor.of(java.lang.foreign.ValueLayout.JAVA_INT,
                        java.lang.foreign.ValueLayout.JAVA_INT));
        System.out.println("abs(-3) = " + (int) abs.invokeExact(-3));
        System.out.println("abs(Integer.MIN_VALUE) = " + (int) abs.invokeExact(Integer.MIN_VALUE));
        System.out.println("Java Math.abs(Integer.MIN_VALUE) = " + Math.abs(Integer.MIN_VALUE));
    }
}
