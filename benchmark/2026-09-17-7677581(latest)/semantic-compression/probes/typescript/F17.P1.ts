declare function require(id: "node:fs"): {
  openSync(path: string, flags: string): number;
  readFileSync(fd: number, encoding: "utf8"): string;
  closeSync(fd: number): void;
};
const fs = require("node:fs");

// BEGIN PROBE F17.P1
function readAll() {
  const fd = fs.openSync("data.txt", "r");
  try {
    const text = fs.readFileSync(fd, "utf8");
    return new TextEncoder().encode(text).length;
  } finally {
    fs.closeSync(fd);
  }
}
// END PROBE F17.P1

console.log(readAll());
