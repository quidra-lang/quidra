"use strict";
const builtins = require("node:module").builtinModules;
console.log("node_builtin_ffi_modules=" + (builtins.filter((m) => /ffi|dlfcn|foreign/i.test(m)).join(",") || "(none)"));
console.log("UNSUPPORTED: no first-party mechanism to pass a struct or a pointer+length array across the C ABI");
