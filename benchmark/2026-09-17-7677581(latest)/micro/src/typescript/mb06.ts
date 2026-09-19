// MB-06 -- Matrix multiplication: classical i-j-k over flat row-major storage,
// n = 512, R = 3.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-06.
// The i-j-k loop order is mandatory: no i-k-j, no tiling, no blocking, no
// transposition of B, no Strassen, no library matmul.
// Data container: Float64Array -- the binary64 array pinned for the `typescript`
// configuration by the binding table in section 4.12(a).
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

const n = 512;
const R = 3;
const SEED = 20266917;

/** Park-Miller / MINSTD generator: state = (48271 * state) mod 2147483647. */
class Lehmer {
  private state: number;

  constructor(seed: number) {
    this.state = seed;
  }

  nextInt(): number {
    this.state = (48271 * this.state) % 2147483647;
    return this.state;
  }

  nextUnit(): number {
    return this.nextInt() / 2147483647.0;
  }
}

interface Mb06Result {
  sumC: number;
  cFirst: number;
  cLast: number;
}

function workload(): Mb06Result {
  const gen = new Lehmer(SEED);
  const a = new Float64Array(n * n);
  const b = new Float64Array(n * n);
  const c = new Float64Array(n * n);
  for (let i = 0; i < n * n; i++) {
    a[i] = gen.nextUnit();
  }
  for (let i = 0; i < n * n; i++) {
    b[i] = gen.nextUnit();
  }

  for (let r = 0; r < R; r++) {
    a[r] = a[r] + 1.0e-9; // anti-elimination (section 2.6)
    for (let i = 0; i < n; i++) {
      for (let j = 0; j < n; j++) {
        let s = 0.0;
        for (let k = 0; k < n; k++) {
          s = s + a[i * n + k] * b[k * n + j];
        }
        c[i * n + j] = s;
      }
    }
  }

  let sumC = 0.0;
  for (let i = 0; i < n * n; i++) {
    sumC = sumC + c[i];
  }

  return { sumC, cFirst: c[0], cLast: c[n * n - 1] };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and regenerates A and B
  // inside the timed region, so it performs exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb06Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = { sumC: 0.0, cFirst: 0.0, cLast: 0.0 };
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

  console.log(
    `MB06 sumC=${res.sumC.toExponential(16)} c_first=${res.cFirst.toExponential(16)}` +
      ` c_last=${res.cLast.toExponential(16)}`,
  );
}

main();
