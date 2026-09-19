declare const __filename: string;
declare function require(id: "node:worker_threads"): {
  Worker: new (file: string, opts: { workerData: SharedArrayBuffer }) => { on(ev: "exit", cb: () => void): void };
  isMainThread: boolean;
  workerData: SharedArrayBuffer;
};
const wt = require("node:worker_threads");

// BEGIN PROBE F19.P2
async function run(): Promise<bigint> {
  const buf = new SharedArrayBuffer(8);
  const counter = new BigInt64Array(buf);
  if (!wt.isMainThread) {
    const shared = new BigInt64Array(wt.workerData);
    for (let i = 0; i < 1000; i++) Atomics.add(shared, 0, 1n);
    return 0n;
  }
  const done = [0, 1].map(() => new Promise<void>(res => {
    new wt.Worker(__filename, { workerData: buf }).on("exit", res);
  }));
  await Promise.all(done);
  return counter[0];
}
// END PROBE F19.P2

run().then(v => { if (wt.isMainThread) console.log(String(v)); });
