// FFI-1 for TypeScript/Node.
// No @types/node is used: those are third-party (DefinitelyTyped), and this probe is
// restricted to first-party features. The two host globals are declared minimally.
declare const process: { dlopen(m: object, p: string): void };
declare function require(id: string): any;

// Search Node.js's own builtin module list for any foreign-function interface.
const builtins: string[] = require("node:module").builtinModules;
const ffiLike = builtins.filter((m: string) => /ffi|dlfcn|foreign|native(?!.*access)/i.test(m));
console.log("node_builtin_ffi_modules=" + (ffiLike.length ? ffiLike.join(",") : "(none)"));

// The only first-party native-loading entry point is process.dlopen, which loads a
// Node-API addon, not an arbitrary C shared library. Try it against the system C library.
try {
  const m: any = { exports: {} };
  process.dlopen(m, "/usr/lib/libSystem.B.dylib");
  console.log("cos(1.0)=" + m.exports.cos(1.0).toFixed(10));
} catch (e: any) {
  console.log("dlopen_libSystem_failed=" + (e && e.message ? e.message : String(e)));
  console.log("cos(1.0)=UNSUPPORTED");
}
