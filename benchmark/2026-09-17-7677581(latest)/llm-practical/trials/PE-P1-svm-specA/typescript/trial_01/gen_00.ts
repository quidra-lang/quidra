// WL-SVM / PE-P1-svm-specA -- soft-margin SVM via dual gradient ascent.
// Standard library only. No linear-algebra / tensor / statistics facility is used;
// every accumulation below is a hand-written scalar loop.
//
// Logical modules: `data`, `svm`, `app`.

// ---------------------------------------------------------------------------
// Frozen constants (spec 2.2)
// ---------------------------------------------------------------------------
const D = 4;
const N_TRAIN_C1 = 100;
const N_TRAIN_C2 = 100;
const N_TEST_C1 = 50;
const N_TEST_C2 = 50;
const MU1: number[] = [1.0, 1.0, 0.5, -0.5];
const MU2: number[] = [-1.0, -1.0, -0.5, 0.5];
const SIGMA = 0.8;
const SEED = 1234567;
const C = 10.0;
const LR = 0.0001;
const LIMIT = 0.0001;
const SWEEPS = 1000;
const EPS_SV = 0.0000001;

const N = N_TRAIN_C1 + N_TRAIN_C2; // 200
const N_TEST = N_TEST_C1 + N_TEST_C2; // 100

// ---------------------------------------------------------------------------
// module `data` -- LCG-PM generator and dataset builder
// ---------------------------------------------------------------------------

/** Park-Miller minimal-standard LCG: state <- (state * 48271) mod 2147483647. */
class LcgPm {
  private state: number;

  constructor(seed: number) {
    this.state = seed;
  }

  /** Advance, then return state / 2147483647.0 in (0,1). */
  nextUnit(): number {
    this.state = (this.state * 48271) % 2147483647;
    return this.state / 2147483647.0;
  }

  /** Irwin-Hall(12) standard normal: sum of 12 unit draws, minus 6. */
  nextNormal(): number {
    let t = 0.0;
    for (let k = 0; k < 12; k++) {
      t = t + this.nextUnit();
    }
    return t - 6.0;
  }
}

/** A block of `count` points of dimension D, drawn point-major, dimension-ascending. */
function drawBlock(rng: LcgPm, count: number, mu: number[]): number[][] {
  const out: number[][] = [];
  for (let n = 0; n < count; n++) {
    const p = new Array<number>(D);
    for (let d = 0; d < D; d++) {
      p[d] = mu[d] + SIGMA * rng.nextNormal();
    }
    out.push(p);
  }
  return out;
}

interface Dataset {
  xTrain: number[][];
  yTrain: number[];
  xTest: number[][];
  yTest: number[];
}

/** Exact draw order of spec 2.3: train+1, train-1, test+1, test-1 from one stream. */
const data = {
  build(): Dataset {
    const rng = new LcgPm(SEED);
    const tr1 = drawBlock(rng, N_TRAIN_C1, MU1);
    const tr2 = drawBlock(rng, N_TRAIN_C2, MU2);
    const te1 = drawBlock(rng, N_TEST_C1, MU1);
    const te2 = drawBlock(rng, N_TEST_C2, MU2);

    const xTrain: number[][] = [];
    const yTrain: number[] = [];
    for (let i = 0; i < tr1.length; i++) {
      xTrain.push(tr1[i]);
      yTrain.push(1.0);
    }
    for (let i = 0; i < tr2.length; i++) {
      xTrain.push(tr2[i]);
      yTrain.push(-1.0);
    }

    const xTest: number[][] = [];
    const yTest: number[] = [];
    for (let i = 0; i < te1.length; i++) {
      xTest.push(te1[i]);
      yTest.push(1.0);
    }
    for (let i = 0; i < te2.length; i++) {
      xTest.push(te2[i]);
      yTest.push(-1.0);
    }

    return { xTrain, yTrain, xTest, yTest };
  },
};

// ---------------------------------------------------------------------------
// module `svm` -- training, support vectors, w/b recovery, f, g, evaluation
// ---------------------------------------------------------------------------

interface Model {
  alpha: Float64Array;
  beta: number;
  judge: boolean;
  errorLast: number;
  maxAbsDelta: number;
  sMargin: number[];
  sInside: number[];
  w: number[];
  b: number;
}

const svm = {
  /** G[i][j] = sum_d ascending x[i][d]*x[j][d], flattened row-major. */
  gram(x: number[][], n: number): Float64Array {
    const g = new Float64Array(n * n);
    for (let i = 0; i < n; i++) {
      const xi = x[i];
      for (let j = 0; j < n; j++) {
        const xj = x[j];
        let s = 0.0;
        for (let d = 0; d < D; d++) {
          s = s + xi[d] * xj[d];
        }
        g[i * n + j] = s;
      }
    }
    return g;
  },

  /** Exactly SWEEPS sweeps of in-place (Gauss-Seidel) dual ascent; no early exit. */
  train(g: Float64Array, y: number[], n: number): Model {
    const alpha = new Float64Array(n); // all 0.0
    let beta = 1.0;

    let judge = false;
    let error = 0.0;
    let maxAbsDelta = 0.0;

    for (let sweep = 0; sweep < SWEEPS; sweep++) {
      judge = false;
      error = 0.0;
      maxAbsDelta = 0.0;

      // (3.1) update alpha, ascending i, in place
      for (let i = 0; i < n; i++) {
        const yi = y[i];
        const row = i * n;
        let item1 = 0.0;
        let item2 = 0.0;
        for (let j = 0; j < n; j++) {
          const aj = alpha[j];
          const sgn = yi * y[j];
          item1 = item1 + aj * sgn * g[row + j];
          item2 = item2 + aj * sgn;
        }

        const delta = 1.0 - item1 - beta * item2;
        const ad = Math.abs(delta);
        if (ad > maxAbsDelta) {
          maxAbsDelta = ad;
        }

        alpha[i] = alpha[i] + LR * delta;
        if (alpha[i] < 0.0) {
          alpha[i] = 0.0;
        } else if (alpha[i] > C) {
          alpha[i] = C;
        } else if (ad > LIMIT) {
          judge = true;
          error = error + (ad - LIMIT);
        }
      }

      // (3.2) update beta, once per sweep
      let s = 0.0;
      for (let i = 0; i < n; i++) {
        s = s + alpha[i] * y[i];
      }
      beta = beta + (s * s) / 2.0;
    }

    const sMargin: number[] = [];
    const sInside: number[] = [];
    for (let i = 0; i < n; i++) {
      if (EPS_SV < alpha[i] && alpha[i] < C - EPS_SV) {
        sMargin.push(i);
      }
      if (alpha[i] >= C - EPS_SV) {
        sInside.push(i);
      }
    }

    return {
      alpha,
      beta,
      judge,
      errorLast: error,
      maxAbsDelta,
      sMargin,
      sInside,
      w: [],
      b: 0.0,
    };
  },

  /** w from margin SVs then inside SVs; b averaged over the margin set. */
  recover(m: Model, x: number[][], y: number[]): void {
    const w = new Array<number>(D);
    for (let d = 0; d < D; d++) {
      w[d] = 0.0;
    }
    for (let d = 0; d < D; d++) {
      for (let k = 0; k < m.sMargin.length; k++) {
        const i = m.sMargin[k];
        w[d] = w[d] + m.alpha[i] * y[i] * x[i][d];
      }
      for (let k = 0; k < m.sInside.length; k++) {
        const i = m.sInside[k];
        w[d] = w[d] + m.alpha[i] * y[i] * x[i][d];
      }
    }

    let b = 0.0;
    for (let k = 0; k < m.sMargin.length; k++) {
      const i = m.sMargin[k];
      let dp = 0.0;
      for (let d = 0; d < D; d++) {
        dp = dp + w[d] * x[i][d];
      }
      b = b + (y[i] - dp);
    }
    b = b / m.sMargin.length;

    m.w = w;
    m.b = b;
  },

  f(m: Model, p: number[]): number {
    let s = 0.0;
    for (let d = 0; d < D; d++) {
      s = s + m.w[d] * p[d];
    }
    return s + m.b;
  },

  g(m: Model, p: number[]): number {
    return svm.f(m, p) >= 0.0 ? 1 : -1;
  },

  countCorrect(m: Model, x: number[][], y: number[], from: number, to: number): number {
    let c = 0;
    for (let i = from; i < to; i++) {
      if (svm.g(m, x[i]) === (y[i] >= 0.0 ? 1 : -1)) {
        c++;
      }
    }
    return c;
  },

  objective(alpha: Float64Array, y: number[], g: Float64Array, n: number): number {
    let sumA = 0.0;
    for (let i = 0; i < n; i++) {
      sumA = sumA + alpha[i];
    }
    let quad = 0.0;
    for (let i = 0; i < n; i++) {
      let inner = 0.0;
      for (let j = 0; j < n; j++) {
        inner = inner + alpha[i] * alpha[j] * y[i] * y[j] * g[i * n + j];
      }
      quad = quad + inner;
    }
    return sumA - 0.5 * quad;
  },
};

// ---------------------------------------------------------------------------
// module `app` -- entry point and output formatting
// ---------------------------------------------------------------------------

function f6(v: number): string {
  return v.toFixed(6);
}

function f12(v: number): string {
  return v.toFixed(12);
}

const app = {
  main(): void {
    const ds = data.build();
    const gram = svm.gram(ds.xTrain, N);
    const model = svm.train(gram, ds.yTrain, N);
    svm.recover(model, ds.xTrain, ds.yTrain);

    const alpha = model.alpha;

    let alphaSum = 0.0;
    for (let i = 0; i < N; i++) {
      alphaSum = alphaSum + alpha[i];
    }
    let alphaYSum = 0.0;
    for (let i = 0; i < N; i++) {
      alphaYSum = alphaYSum + alpha[i] * ds.yTrain[i];
    }
    let alphaChecksum = 0.0;
    for (let i = 0; i < N; i++) {
      alphaChecksum = alphaChecksum + alpha[i] * ((i % 97) + 1);
    }

    const objective = svm.objective(alpha, ds.yTrain, gram, N);

    const trainCorrect = svm.countCorrect(model, ds.xTrain, ds.yTrain, 0, N);
    const testC1 = svm.countCorrect(model, ds.xTest, ds.yTest, 0, N_TEST_C1);
    const testC2 = svm.countCorrect(model, ds.xTest, ds.yTest, N_TEST_C1, N_TEST);
    const testTotal = testC1 + testC2;

    const preds: number[] = [];
    for (let i = 0; i < N_TEST; i++) {
      preds.push(svm.g(model, ds.xTest[i]));
    }

    const lines: string[] = [];
    lines.push("SVM_VERSION 1");
    lines.push("SWEEPS " + SWEEPS);
    lines.push("CONVERGED " + (model.judge ? 0 : 1));
    lines.push("ERROR_LAST " + f6(model.errorLast));
    lines.push("MAX_ABS_DELTA " + f6(model.maxAbsDelta));
    lines.push("BETA " + f6(model.beta));
    lines.push("NS_MARGIN " + model.sMargin.length);
    lines.push("NS_INSIDE " + model.sInside.length);
    lines.push("W " + model.w.map(f6).join(" "));
    lines.push("B " + f6(model.b));
    lines.push("OBJECTIVE " + f6(objective));
    lines.push("ALPHA_SUM " + f6(alphaSum));
    lines.push("ALPHA_Y_SUM " + f12(alphaYSum));
    lines.push("ALPHA_CHECKSUM " + f6(alphaChecksum));
    lines.push("TRAIN_ACC " + f6(trainCorrect / N));
    lines.push("TEST_ACC " + f6(testTotal / N_TEST));
    lines.push("TEST_ACC_C1 " + f6(testC1 / N_TEST_C1));
    lines.push("TEST_ACC_C2 " + f6(testC2 / N_TEST_C2));
    lines.push("TEST_CORRECT " + testC1 + " " + testC2 + " " + testTotal);
    lines.push("PRED " + preds.join(" "));
    const alphaOut: string[] = [];
    for (let i = 0; i < N; i++) {
      alphaOut.push(f6(alpha[i]));
    }
    lines.push("ALPHA " + alphaOut.join(" "));

    console.log(lines.join("\n"));
  },
};

app.main();
