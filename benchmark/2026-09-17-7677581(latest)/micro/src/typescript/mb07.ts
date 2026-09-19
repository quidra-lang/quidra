// MB-07 -- Sorting: bottom-up iterative merge sort, ascending, stable,
// ping-pong buffers. N = 2000000, R = 4.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-07.
// The pinned merge sort only: no Array.prototype.sort, no insertion-sort
// cut-off, no radix sort, no early exit. The per-round copy of the pristine
// input is part of the workload and is not elided.
// Data container: Float64Array -- the 64-bit integer array pinned for the
// `typescript` configuration by the binding table in section 4.12(a), which
// names this workload and states that `Int32Array` is not used. Every element
// stays within [1, 2147483647] and ssum <= 2.15e15, both exact in binary64.
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

const N = 2000000;
const R = 4;
const SEED = 20267917;

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

/** Bottom-up merge sort of `a`, using `buf` as the alternate ping-pong buffer. */
function msort(a: Float64Array, buf: Float64Array, n: number): void {
  let src = a;
  let dst = buf;
  let width = 1;
  while (width < n) {
    let lo = 0;
    while (lo < n) {
      const mid = Math.min(lo + width, n);
      const hi = Math.min(lo + 2 * width, n);
      let i = lo;
      let j = mid;
      let k = lo;
      while (i < mid && j < hi) {
        if (src[i] <= src[j]) {
          dst[k] = src[i];
          i++;
        } else {
          dst[k] = src[j];
          j++;
        }
        k++;
      }
      while (i < mid) {
        dst[k] = src[i];
        i++;
        k++;
      }
      while (j < hi) {
        dst[k] = src[j];
        j++;
        k++;
      }
      lo += 2 * width;
    }
    const tmp = src;
    src = dst;
    dst = tmp;
    width *= 2;
  }
  if (src !== a) {
    a.set(src);
  }
}

interface Mb07Result {
  total: number;
  ssum: number;
  inv: number;
}

function workload(): Mb07Result {
  const gen = new Lehmer(SEED);
  const src = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    src[i] = gen.nextInt();
  }
  const buf = new Float64Array(N);

  let total = 0;
  let ssum = 0;
  let inv = 0;
  for (let r = 0; r < R; r++) {
    src[r] = src[r] + 1; // anti-elimination (section 2.6)
    const a = src.slice(); // full element-wise copy, counted
    msort(a, buf, N);

    let chk = 0;
    for (let i = 0; i < N; i++) {
      chk = (chk * 31 + (a[i] % 1000003)) % 1000003;
    }
    total = (total * 7 + chk) % 1000003;

    ssum = 0;
    for (let i = 0; i < N; i++) {
      ssum = ssum + a[i];
    }
    for (let i = 1; i < N; i++) {
      if (a[i - 1] > a[i]) {
        inv = inv + 1;
      }
    }
  }

  return { total, ssum, inv };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and regenerates `src`
  // inside the timed region, so it performs exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb07Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = { total: 0, ssum: 0, inv: 0 };
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

  console.log(`MB07 ${res.total} ${res.ssum} ${res.inv}`);
}

main();
