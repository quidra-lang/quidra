"use strict";
// Search Node.js's own builtin module list for any foreign-function interface.
const builtins = require("node:module").builtinModules;
const ffiLike = builtins.filter((m) => /ffi|dlfcn|foreign|native(?!.*access)/i.test(m));
console.log("node_builtin_ffi_modules=" + (ffiLike.length ? ffiLike.join(",") : "(none)"));
// The only first-party native-loading entry point is process.dlopen, which loads a
// Node-API addon, not an arbitrary C shared library. Try it against the system C library.
try {
    const m = { exports: {} };
    process.dlopen(m, "/usr/lib/libSystem.B.dylib");
    console.log("cos(1.0)=" + m.exports.cos(1.0).toFixed(10));
}
catch (e) {
    console.log("dlopen_libSystem_failed=" + (e && e.message ? e.message : String(e)));
    console.log("cos(1.0)=UNSUPPORTED");
}
