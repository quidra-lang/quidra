declare function require(id: "node:fs"): { openSync(p: string, f: string): number; };
const fs = require("node:fs");
try { fs.openSync("missing.txt", "r"); } catch (e) { console.log("open failed: " + String((e as Error).message)); }
