// FFI-3 for TypeScript/Node. Requires a C boundary; see ffi1.ts for the search that
// establishes Node.js and tsc expose no first-party foreign-function interface.
declare function require(id: string): any;
const builtins: string[] = require("node:module").builtinModules;
console.log("node_builtin_ffi_modules=" + (builtins.filter((m: string) => /ffi|dlfcn|foreign/i.test(m)).join(",") || "(none)"));
console.log("UNSUPPORTED: no first-party mechanism to pass a struct or a pointer+length array across the C ABI");
