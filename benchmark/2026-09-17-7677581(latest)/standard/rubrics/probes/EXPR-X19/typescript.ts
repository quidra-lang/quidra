// Node worker_threads: a standard-library concurrency primitive of the toolchain
// under test (tsc 7.0.2 + node v24.2.0). No external package is used.
declare function require(m: string): any;
declare const module: any;
const wt = require("node:worker_threads");
if (wt.isMainThread) {
  const w = new wt.Worker(module.filename);
  w.on("message", (m: number) => { console.log("X19", m); });
} else { wt.parentPort.postMessage(42); }
