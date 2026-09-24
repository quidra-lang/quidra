// MB-04 -- Floating-point arithmetic: 4-accumulator FP mix, M = 4000, R = 75000.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-04.
// Four separate scalar accumulators, accumulated in the pinned order; no Kahan
// summation, no partial sums, no reciprocal or rsqrt substitution.
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

const M = 4000;
const R = 75000;
const SEED = 20264917;

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

interface Mb04Result {
  s1: number;
  s2: number;
  s3: number;
  s4: number;
}

function workload(): Mb04Result {
  const gen = new Lehmer(SEED);
  const a = new Float64Array(M);
  const b = new Float64Array(M);
  for (let i = 0; i < M; i++) {
    a[i] = 0.5 + gen.nextUnit();
  }
  for (let i = 0; i < M; i++) {
    b[i] = 0.5 + gen.nextUnit();
  }

  let s1 = 0.0;
  let s2 = 0.0;
  let s3 = 0.0;
  let s4 = 0.0;
  for (let r = 0; r < R; r++) {
    a[r % M] = a[r % M] + 1.0e-9; // anti-elimination (section 2.6)
    for (let i = 0; i < M; i++) {
      const av = a[i];
      const bv = b[i];
      s1 = s1 + av * bv;
      s2 = s2 + av / (bv + 2.0);
      s3 = s3 + Math.sqrt(av * av + bv * bv);
      s4 = s4 + (av - bv) * (av - bv);
    }
  }

  return { s1, s2, s3, s4 };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator and regenerates A and B, so it
  // performs exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb04Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = { s1: 0.0, s2: 0.0, s3: 0.0, s4: 0.0 };
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
    `MB04 s1=${res.s1.toExponential(16)} s2=${res.s2.toExponential(16)}` +
      ` s3=${res.s3.toExponential(16)} s4=${res.s4.toExponential(16)}`,
  );
}

main();
