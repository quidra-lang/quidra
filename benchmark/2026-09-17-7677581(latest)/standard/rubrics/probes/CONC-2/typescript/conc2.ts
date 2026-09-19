// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
// The counter lives in a SharedArrayBuffer shared by two node:worker_threads Workers
// and is incremented with a plain non-atomic read-modify-write.
declare function require(id: string): any;
declare const __filename: string;

const wt: any = require("node:worker_threads");

const ITERS = 200000;

if (wt.isMainThread) {
  const sab = new SharedArrayBuffer(4);
  const view = new Int32Array(sab);
  let left = 2;
  for (let k = 0; k < 2; k++) {
    const w = new wt.Worker(__filename, { workerData: { sab: sab } });
    w.on("exit", () => {
      left--;
      if (left === 0) console.log("counter=" + view[0] + " expected=" + 2 * ITERS);
    });
  }
} else {
  const view = new Int32Array(wt.workerData.sab);
  for (let i = 0; i < ITERS; i++) view[0] = view[0] + 1;
}
