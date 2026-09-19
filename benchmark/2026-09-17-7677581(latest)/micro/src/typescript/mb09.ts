// MB-09 -- Statistics: two-pass moments, Pearson correlation, histogram.
// N = 2000000, R = 30.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-09.
// Five separate passes, in the pinned order, with the pinned accumulation order:
// no Welford / single-pass substitution, no pass fusion, no reassociation, and
// the histogram bin is floor(x * 0.64) with the two clamps.
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
const R = 30;
const SEED = 20269917;

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

interface Mb09Result {
  mean: number;
  variance: number;
  sd: number;
  mn: number;
  mx: number;
  mad: number;
  pearson: number;
  histChk: number;
}

function workload(): Mb09Result {
  const gen = new Lehmer(SEED);
  const x = new Float64Array(N);
  const y = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    x[i] = gen.nextUnit() * 100.0;
  }
  for (let i = 0; i < N; i++) {
    y[i] = gen.nextUnit() * 100.0;
  }

  const hist = new Int32Array(64);
  let mean = 0.0;
  let variance = 0.0;
  let sd = 0.0;
  let mad = 0.0;
  let mn = 0.0;
  let mx = 0.0;
  let pearson = 0.0;
  let histChk = 0;

  for (let r = 0; r < R; r++) {
    x[r] = x[r] + 1.0e-9; // anti-elimination (section 2.6)

    let s = 0.0; // pass 1: sum, min, max
    mn = x[0];
    mx = x[0];
    for (let i = 0; i < N; i++) {
      const v = x[i];
      s = s + v;
      if (v < mn) {
        mn = v;
      }
      if (v > mx) {
        mx = v;
      }
    }
    mean = s / N;

    let sq = 0.0; // pass 2: variance and mean absolute deviation
    let ad = 0.0;
    for (let i = 0; i < N; i++) {
      const d = x[i] - mean;
      sq = sq + d * d;
      ad = ad + (d < 0 ? -d : d);
    }
    variance = sq / N;
    sd = Math.sqrt(variance);
    mad = ad / N;

    let sy = 0.0; // pass 3: mean of Y
    for (let i = 0; i < N; i++) {
      sy = sy + y[i];
    }
    const meany = sy / N;

    let sxy = 0.0; // pass 4: Pearson correlation, two-pass form
    let sxx = 0.0;
    let syy = 0.0;
    for (let i = 0; i < N; i++) {
      const dx = x[i] - mean;
      const dy = y[i] - meany;
      sxy = sxy + dx * dy;
      sxx = sxx + dx * dx;
      syy = syy + dy * dy;
    }
    pearson = sxy / Math.sqrt(sxx * syy);

    hist.fill(0); // pass 5: 64-bin histogram
    for (let i = 0; i < N; i++) {
      // `| 0` is the JavaScript spelling of the pseudocode's floor_to_int: the
      // bin index is an integer everywhere else in the suite, and 0 <= b <= 63
      // here, so this changes no value.
      let b = Math.floor(x[i] * 0.64) | 0;
      if (b < 0) {
        b = 0;
      }
      if (b > 63) {
        b = 63;
      }
      hist[b] = hist[b] + 1;
    }
    histChk = 0;
    for (let b = 0; b < 64; b++) {
      histChk = histChk + (b + 1) * hist[b];
    }
  }

  return { mean, variance, sd, mn, mx, mad, pearson, histChk };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and regenerates X and Y
  // inside the timed region, so it performs exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb09Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = {
      mean: 0.0,
      variance: 0.0,
      sd: 0.0,
      mn: 0.0,
      mx: 0.0,
      mad: 0.0,
      pearson: 0.0,
      histChk: 0,
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
    `MB09 mean=${res.mean.toExponential(16)} var=${res.variance.toExponential(16)}` +
      ` sd=${res.sd.toExponential(16)} min=${res.mn.toExponential(16)}` +
      ` max=${res.mx.toExponential(16)} mad=${res.mad.toExponential(16)}` +
      ` pearson=${res.pearson.toExponential(16)} hist_chk=${res.histChk}`,
  );
}

main();
