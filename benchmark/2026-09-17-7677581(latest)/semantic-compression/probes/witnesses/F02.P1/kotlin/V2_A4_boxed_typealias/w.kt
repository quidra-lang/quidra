// WITNESS V2 for F02.P1 / kotlin, Rule 1.3.1.
// The measured fragment (between the markers) is BYTE-IDENTICAL to the frozen probe.
// A single top-level typealias declared OUTSIDE the fragment rebinds the simple name `Int`,
// which reaches the fragment only through Kotlin's default `kotlin.*` import; a same-file
// top-level declaration takes resolution precedence over a default import.
// Observed: A4 moves from "fixed-width signed integer" to "boxed/reference-typed numeric",
// and A8 moves from "none observable" to "allocation may occur" (the literal is boxed).
typealias Int = Number

fun probe(cond: Boolean): Int {
    // BEGIN PROBE F02.P1
    val v: Int
    if (cond) v = 5 else v = 9
    return v
    // END PROBE F02.P1
}

fun main() {
    val r = probe(true)
    println("value=" + r + " runtime_class=" + (r as Any).javaClass.name)
}
