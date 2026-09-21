// MB-05 -- Vector inner product: straight-line dot product, N = 2000000, R = 400.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-05.
// One accumulator, one straight ascending loop; no partial-sum unrolling, no
// Kahan summation, no reduce/zip rewrite that would change accumulation order.
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

const N = 2000000;
const R = 400;
const SEED = 20265917;

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

function workload(): number {
  const gen = new Lehmer(SEED);
  const x = new Float64Array(N);
  const y = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    x[i] = 0.5 + gen.nextUnit();
  }
  for (let i = 0; i < N; i++) {
    y[i] = 0.5 + gen.nextUnit();
  }

  let total = 0.0;
  for (let r = 0; r < R; r++) {
    x[r] = x[r] + 1.0e-9; // anti-elimination (section 2.6); R <= N so no wrap
    let d = 0.0;
    for (let i = 0; i < N; i++) {
      d = d + x[i] * y[i];
    }
    total = total + d;
  }
  return total;
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and regenerates X and Y
  // inside the timed region, so it performs exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let total = 0.0;
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

  console.log(`MB05 total=${total.toExponential(16)}`);
}

main();
