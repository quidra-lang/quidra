// CONC-1, TypeScript on Node.js. Worker mechanism: node:worker_threads Worker
// (a Node.js built-in module; Node.js is in this language's official project set).
// No @types/node is used: those are third-party (DefinitelyTyped). The few host
// globals this probe needs are declared minimally, as in the other probes.
declare function require(id: string): any;
declare const __filename: string;
declare const process: { argv: string[] };

const wt: any = require("node:worker_threads");

const N: number = 500000000;
const CHUNKS: number = 4;
const SPAN: number = N / CHUNKS;

function chunkSum(c: number): number {
  let s = 0.0;
  const start = c * SPAN;
  const end = start + SPAN;
  for (let i = start; i < end; i++) { const x = Math.sin(i); s += x * x; }
  return s;
}

if (wt.isMainThread) {
  const W: number = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 1;
  const partial: number[] = new Array(CHUNKS).fill(0);
  let left = W;
  for (let w = 0; w < W; w++) {
    const worker = new wt.Worker(__filename, { workerData: { w: w, W: W } });
    worker.on("message", (msg: Array<[number, number]>) => {
      for (const [c, v] of msg) partial[c] = v;
      left--;
      if (left === 0) {
        let total = 0.0;
        for (let c = 0; c < CHUNKS; c++) total = total + partial[c];
        console.log("workers=" + W + " result=" + total.toFixed(10));
      }
    });
  }
} else {
  const { w, W } = wt.workerData;
  const out: Array<[number, number]> = [];
  for (let c = 0; c < CHUNKS; c++) if (c % W === w) out.push([c, chunkSum(c)]);
  wt.parentPort.postMessage(out);
}
