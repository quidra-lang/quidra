// MB-02 -- Factorial: recompute k! mod M for every k, N = 20000, M = 1000003.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-02.
// The inner loop restarts from f = 1 for every k; f is never carried across k.
// No caching of partial factorials, no closed form, no modular exponentiation.
//
// Representability: f <= M - 1 = 1000002 and j <= 20000, so f * j <= 2.0e10,
// which is exact in a binary64 `number`.
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

const M = 1000003;
const N = 20000;

function workload(): number {
  let total = 0;
  for (let k = 1; k <= N; k++) {
    let f = 1;
    for (let j = 2; j <= k; j++) {
      f = (f * j) % M;
    }
    total = (total + f) % M;
  }
  return total;
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let total = 0;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    for (let k = 0; k < iterations; k++) {
      const t0 = process.hrtime.bigint();
      total = workload();
      const elapsed = process.hrtime.bigint() - t0;
      // fs.writeSync(1, ..) is Node's synchronous stdout write: section 5.2
      // requires each ITER line to be flushed immediately.
      fs.writeSync(1, `ITER ${k} ${elapsed}\n`);
    }
  } else {
    total = workload();
  }
  console.log(`MB02 ${total}`);
}

main();
