declare function require(id: "node:fs"): {
  openSync(path: string, flags: string): number;
  readFileSync(fd: number, encoding: "utf8"): string;
  closeSync(fd: number): void;
  fstatSync(fd: number): unknown;
};
const fs = require("node:fs");

function readAllFailing(): number {
  const fd = fs.openSync("data.txt", "r");
  try {
    const text = fs.readFileSync(fd + 900, "utf8");
    return new TextEncoder().encode(text).length;
  } finally {
    console.log("finally ran, closing fd " + String(fd));
    fs.closeSync(fd);
    try { fs.fstatSync(fd); console.log("fd STILL OPEN"); }
    catch (e) { console.log("fd closed: " + String((e as Error).message)); }
  }
}
try { readAllFailing(); } catch (e) { console.log("propagated: " + String((e as Error).message)); }
