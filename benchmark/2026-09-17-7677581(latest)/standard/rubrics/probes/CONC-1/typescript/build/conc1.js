"use strict";
const wt = require("node:worker_threads");
const N = 500000000;
const CHUNKS = 4;
const SPAN = N / CHUNKS;
function chunkSum(c) {
    let s = 0.0;
    const start = c * SPAN;
    const end = start + SPAN;
    for (let i = start; i < end; i++) {
        const x = Math.sin(i);
        s += x * x;
    }
    return s;
}
if (wt.isMainThread) {
    const W = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 1;
    const partial = new Array(CHUNKS).fill(0);
    let left = W;
    for (let w = 0; w < W; w++) {
        const worker = new wt.Worker(__filename, { workerData: { w: w, W: W } });
        worker.on("message", (msg) => {
            for (const [c, v] of msg)
                partial[c] = v;
            left--;
            if (left === 0) {
                let total = 0.0;
                for (let c = 0; c < CHUNKS; c++)
                    total = total + partial[c];
                console.log("workers=" + W + " result=" + total.toFixed(10));
            }
        });
    }
}
else {
    const { w, W } = wt.workerData;
    const out = [];
    for (let c = 0; c < CHUNKS; c++)
        if (c % W === w)
            out.push([c, chunkSum(c)]);
    wt.parentPort.postMessage(out);
}
