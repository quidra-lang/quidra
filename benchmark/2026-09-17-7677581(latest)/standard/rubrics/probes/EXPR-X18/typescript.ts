// tsc ships no type declarations for the Node runtime named in TypeScript's own
// frozen run recipe, so the CommonJS entry point is declared locally. No package
// outside the toolchain distribution (tsc 7.0.2 + node v24.2.0) is used.
declare function require(m: string): any;
const fs = require("node:fs");
fs.writeFileSync("x18.txt", "hello");
const d: string = fs.readFileSync("x18.txt", "utf8");
console.log("X18", d, fs.existsSync("x18.txt"));
