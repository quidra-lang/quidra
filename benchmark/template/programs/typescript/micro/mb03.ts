// MB-03 -- Integer arithmetic: 5-operation integer mix, N = 120000000.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-03.
// All five statements appear in the pinned order with the pinned constants.
// No Barrett/Montgomery reduction, no shift/add replacement for `mod 2147483647`,
// no multiply-shift replacement for idiv(x, 1000).
//
// Representability (section 2.1): every value stays below 2^53, so plain
// `number` is exact throughout. Both operands of the XOR are < 2^31, so
// JavaScript's int32 `^` yields the same non-negative result as a 64-bit XOR.
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

const N = 120000000;
const SEED = 20263917;

interface Mb03Result {
  sAdd: number;
  sXor: number;
  sMul: number;
  sDiv: number;
}

function workload(): Mb03Result {
  let x = SEED;
  let sAdd = 0;
  let sXor = 0;
  let sMul = 1;
  let sDiv = 0;

  for (let i = 0; i < N; i++) {
    x = (48271 * x) % 2147483647;
    sAdd = (sAdd + x) % 2147483647;
    sXor = sXor ^ x;
    sMul = (sMul * 33 + (x % 97)) % 1000003;
    sDiv = sDiv + Math.floor(x / 1000);
  }

  return { sAdd, sXor, sMul, sDiv };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb03Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = { sAdd: 0, sXor: 0, sMul: 1, sDiv: 0 };
    for (let k = 0; k < iterations; k++) {
      const t0 = process.hrtime.bigint();
      res = workload();
      const elapsed = process.hrtime.bigint() - t0;
      // fs.writeSync(1, ..) is Node's synchronous stdout write: section 5.2
      // requires each ITER line to be flushed immediately.
      fs.writeSync(1, `ITER ${k} ${elapsed}\n`);
    }
  } else {
    res = workload();
  }
  console.log(`MB03 ${res.sAdd} ${res.sXor} ${res.sMul} ${res.sDiv}`);
}

main();
