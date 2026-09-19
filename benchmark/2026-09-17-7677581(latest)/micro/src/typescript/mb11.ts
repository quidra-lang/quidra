// MB-11 -- Collections: the standard associative container, set and dynamic
// array under insert / update / lookup / delete / iterate.
// N = 1000000, R = 3.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-11.
// TypeScript's standard general-purpose containers are used: Map, Set and Array
// (section 4.12(d)). Default construction only -- no presizing, no capacity
// hint, no custom hash. Fresh containers every round, as the pseudocode
// constructs them inside the loop.
// `keys` is a Float64Array -- the 64-bit integer array pinned for the
// `typescript` configuration by the binding table in section 4.12(a), which
// states that `Int32Array` is not used. This also removes the int32 wrap hazard
// in the `keys[r] + 1000000` anti-elimination step.
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

const N = 1000000;
const R = 3;
const KM = 500009;
const SM = 100003;
const Q = 1000000007;
const SEED = 20271917;

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
}

interface Mb11Result {
  acc: number;
  size1: number;
  found: number;
  vsum: number;
  mchk: number;
  size2: number;
  size3: number;
  lsum: number;
}

function workload(): Mb11Result {
  const gen = new Lehmer(SEED);
  const keys = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    keys[i] = gen.nextInt();
  }

  let acc = 0;
  let size1 = 0;
  let found = 0;
  let vsum = 0;
  let mchk = 0;
  let size2 = 0;
  let size3 = 0;
  let lsum = 0;

  for (let r = 0; r < R; r++) {
    keys[r] = keys[r] + 1000000; // anti-elimination (section 2.6)

    const m = new Map<number, number>();
    for (let i = 0; i < N; i++) {
      const k = keys[i] % KM;
      const prev = m.get(k);
      m.set(k, (prev === undefined ? 0 : prev) + 1);
    }
    size1 = m.size;

    found = 0;
    vsum = 0;
    for (let i = 0; i < N; i++) {
      const k = (keys[i] + 7) % KM;
      const v = m.get(k);
      if (v !== undefined) {
        found = found + 1;
        vsum = vsum + v;
      }
    }

    mchk = 0;
    for (const [k, v] of m) {
      mchk = (mchk + (k % 1000003) * v) % 1000003;
    }

    for (let i = 0; i < N; i += 2) {
      const k = keys[i] % KM;
      if (m.has(k)) {
        m.delete(k);
      }
    }
    size2 = m.size;

    const st = new Set<number>();
    for (let i = 0; i < N; i++) {
      st.add(keys[i] % SM);
    }
    size3 = st.size;

    const lst: number[] = [];
    for (let i = 0; i < N; i++) {
      lst.push(keys[i] % 1000);
    }
    lsum = 0;
    for (const v of lst) {
      lsum = lsum + v;
    }

    for (const v of [size1, found, vsum, mchk, size2, size3, lsum]) {
      acc = (acc * 31 + v) % Q;
    }
  }

  return { acc, size1, found, vsum, mchk, size2, size3, lsum };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and regenerates `keys`
  // inside the timed region, so it performs exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb11Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = {
      acc: 0,
      size1: 0,
      found: 0,
      vsum: 0,
      mchk: 0,
      size2: 0,
      size3: 0,
      lsum: 0,
    };
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
    `MB11 ${res.acc} ${res.size1} ${res.found} ${res.vsum} ${res.mchk}` +
      ` ${res.size2} ${res.size3} ${res.lsum}`,
  );
}

main();
