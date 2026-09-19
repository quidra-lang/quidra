// WL-SVM / PE-P1-svm-specA — soft-margin SVM by dual gradient ascent.
//
// Standard library only. Single threaded. No linear-algebra / tensor / statistics / BLAS
// facility of any kind is used: every accumulation below is a hand-written loop over plain
// arrays, per frozen rule C-9.
//
// Modules: `data` (LCG-PM generator + dataset builder), `svm` (training, SV extraction,
// w/b recovery, f, g, evaluation), `app` (entry point and output formatting).

// ---------------------------------------------------------------------------
// module data
// ---------------------------------------------------------------------------
mod data {
    pub const D: usize = 4;
    pub const N_TRAIN_C1: usize = 100;
    pub const N_TRAIN_C2: usize = 100;
    pub const N_TEST_C1: usize = 50;
    pub const N_TEST_C2: usize = 50;
    pub const MU1: [f64; D] = [1.0, 1.0, 0.5, -0.5];
    pub const MU2: [f64; D] = [-1.0, -1.0, -0.5, 0.5];
    pub const SIGMA: f64 = 0.8;
    pub const SEED: i64 = 1234567;

    const MULT: i64 = 48271;
    const MODULUS: i64 = 2147483647;

    /// Park-Miller minimal-standard Lehmer generator, exact integer arithmetic.
    pub struct LcgPm {
        state: i64,
    }

    impl LcgPm {
        pub fn new(seed: i64) -> LcgPm {
            LcgPm { state: seed }
        }

        pub fn next_state(&mut self) -> i64 {
            self.state = (MULT * self.state) % MODULUS;
            self.state
        }

        pub fn next_uniform(&mut self) -> f64 {
            (self.next_state() as f64) / 2147483647.0
        }

        /// Irwin-Hall(12) minus 6; ascending accumulation, never reassociated.
        pub fn next_normal(&mut self) -> f64 {
            let mut t = 0.0f64;
            for _ in 0..12 {
                t = t + self.next_uniform();
            }
            t - 6.0
        }
    }

    pub struct Dataset {
        pub x_train: Vec<[f64; D]>,
        pub y_train: Vec<f64>,
        pub x_test: Vec<[f64; D]>,
        pub y_test: Vec<f64>,
    }

    fn draw_block(rng: &mut LcgPm, n: usize, mu: &[f64; D]) -> Vec<[f64; D]> {
        let mut out: Vec<[f64; D]> = Vec::with_capacity(n);
        for _n in 0..n {
            let mut p = [0.0f64; D];
            for d in 0..D {
                p[d] = mu[d] + SIGMA * rng.next_normal();
            }
            out.push(p);
        }
        out
    }

    /// One stream, seeded once, drawn in the pinned order:
    /// train +1, train -1, test +1, test -1.
    pub fn build() -> Dataset {
        let mut rng = LcgPm::new(SEED);
        let tr1 = draw_block(&mut rng, N_TRAIN_C1, &MU1);
        let tr2 = draw_block(&mut rng, N_TRAIN_C2, &MU2);
        let te1 = draw_block(&mut rng, N_TEST_C1, &MU1);
        let te2 = draw_block(&mut rng, N_TEST_C2, &MU2);

        let mut x_train: Vec<[f64; D]> = Vec::with_capacity(N_TRAIN_C1 + N_TRAIN_C2);
        let mut y_train: Vec<f64> = Vec::with_capacity(N_TRAIN_C1 + N_TRAIN_C2);
        for p in tr1.iter() {
            x_train.push(*p);
            y_train.push(1.0);
        }
        for p in tr2.iter() {
            x_train.push(*p);
            y_train.push(-1.0);
        }

        let mut x_test: Vec<[f64; D]> = Vec::with_capacity(N_TEST_C1 + N_TEST_C2);
        let mut y_test: Vec<f64> = Vec::with_capacity(N_TEST_C1 + N_TEST_C2);
        for p in te1.iter() {
            x_test.push(*p);
            y_test.push(1.0);
        }
        for p in te2.iter() {
            x_test.push(*p);
            y_test.push(-1.0);
        }

        Dataset {
            x_train,
            y_train,
            x_test,
            y_test,
        }
    }
}

// ---------------------------------------------------------------------------
// module svm
// ---------------------------------------------------------------------------
mod svm {
    use crate::data::D;

    pub const C: f64 = 10.0;
    pub const LR: f64 = 0.0001;
    pub const LIMIT: f64 = 0.0001;
    pub const SWEEPS: usize = 1000;
    pub const EPS_SV: f64 = 0.0000001;

    pub struct Trained {
        pub alpha: Vec<f64>,
        pub beta: f64,
        pub judge_last: bool,
        pub error_last: f64,
        pub max_abs_delta_last: f64,
        pub gram: Vec<f64>,
        pub n: usize,
    }

    pub struct Model {
        pub w: [f64; D],
        pub b: f64,
        pub ns_margin: usize,
        pub ns_inside: usize,
    }

    /// Mandatory one-off Gram build: G[i][j] = sum_d x[i][d]*x[j][d], d ascending.
    fn gram_build(x: &[[f64; D]]) -> Vec<f64> {
        let n = x.len();
        let mut g = vec![0.0f64; n * n];
        for i in 0..n {
            for j in 0..n {
                let mut s = 0.0f64;
                for d in 0..D {
                    s = s + x[i][d] * x[j][d];
                }
                g[i * n + j] = s;
            }
        }
        g
    }

    /// Exactly SWEEPS sweeps, no early exit. Gauss-Seidel in-place alpha update.
    pub fn train(x: &[[f64; D]], y: &[f64]) -> Trained {
        let n = x.len();
        let g = gram_build(x);

        let mut alpha = vec![0.0f64; n];
        let mut beta = 1.0f64;

        let mut judge = false;
        let mut error = 0.0f64;
        let mut max_abs_delta = 0.0f64;

        for _sweep in 0..SWEEPS {
            judge = false;
            error = 0.0;
            max_abs_delta = 0.0;

            // (3.1) update alpha, ascending i, in place
            for i in 0..n {
                let yi = y[i];
                let base = i * n;

                let mut item1 = 0.0f64;
                for j in 0..n {
                    item1 = item1 + alpha[j] * yi * y[j] * g[base + j];
                }

                let mut item2 = 0.0f64;
                for j in 0..n {
                    item2 = item2 + alpha[j] * yi * y[j];
                }

                let delta = 1.0 - item1 - beta * item2;
                let ad = if delta < 0.0 { -delta } else { delta };

                if ad > max_abs_delta {
                    max_abs_delta = ad;
                }

                alpha[i] = alpha[i] + LR * delta;
                if alpha[i] < 0.0 {
                    alpha[i] = 0.0;
                } else if alpha[i] > C {
                    alpha[i] = C;
                } else if ad > LIMIT {
                    judge = true;
                    error = error + (ad - LIMIT);
                }
            }

            // (3.2) update beta, once per sweep
            let mut s = 0.0f64;
            for i in 0..n {
                s = s + alpha[i] * y[i];
            }
            beta = beta + s * s / 2.0;
        }

        Trained {
            alpha,
            beta,
            judge_last: judge,
            error_last: error,
            max_abs_delta_last: max_abs_delta,
            gram: g,
            n,
        }
    }

    pub fn support_sets(alpha: &[f64]) -> (Vec<usize>, Vec<usize>) {
        let mut s_margin: Vec<usize> = Vec::new();
        let mut s_inside: Vec<usize> = Vec::new();
        for i in 0..alpha.len() {
            if EPS_SV < alpha[i] && alpha[i] < C - EPS_SV {
                s_margin.push(i);
            }
            if alpha[i] >= C - EPS_SV {
                s_inside.push(i);
            }
        }
        (s_margin, s_inside)
    }

    pub fn recover(x: &[[f64; D]], y: &[f64], alpha: &[f64]) -> Model {
        let (s_margin, s_inside) = support_sets(alpha);

        let mut w = [0.0f64; D];
        for d in 0..D {
            for &i in s_margin.iter() {
                w[d] = w[d] + alpha[i] * y[i] * x[i][d];
            }
            for &i in s_inside.iter() {
                w[d] = w[d] + alpha[i] * y[i] * x[i][d];
            }
        }

        let mut b = 0.0f64;
        for &i in s_margin.iter() {
            let mut dp = 0.0f64;
            for d in 0..D {
                dp = dp + w[d] * x[i][d];
            }
            b = b + (y[i] - dp);
        }
        if s_margin.len() == 0 {
            // 2.7: |S_margin| == 0 is a FAIL condition; cannot recover b.
            eprintln!("FATAL: empty margin support set");
            std::process::exit(1);
        }
        b = b / (s_margin.len() as f64);

        Model {
            w,
            b,
            ns_margin: s_margin.len(),
            ns_inside: s_inside.len(),
        }
    }

    pub fn f(m: &Model, p: &[f64; D]) -> f64 {
        let mut s = 0.0f64;
        for d in 0..D {
            s = s + m.w[d] * p[d];
        }
        s + m.b
    }

    pub fn g_sign(m: &Model, p: &[f64; D]) -> i32 {
        if f(m, p) >= 0.0 {
            1
        } else {
            -1
        }
    }

    pub fn objective(alpha: &[f64], y: &[f64], gram: &[f64], n: usize) -> f64 {
        let mut sum_alpha = 0.0f64;
        for i in 0..n {
            sum_alpha = sum_alpha + alpha[i];
        }
        let mut quad = 0.0f64;
        for i in 0..n {
            let mut inner = 0.0f64;
            for j in 0..n {
                inner = inner + alpha[i] * alpha[j] * y[i] * y[j] * gram[i * n + j];
            }
            quad = quad + inner;
        }
        sum_alpha - 0.5 * quad
    }

    pub fn alpha_sum(alpha: &[f64]) -> f64 {
        let mut s = 0.0f64;
        for i in 0..alpha.len() {
            s = s + alpha[i];
        }
        s
    }

    pub fn alpha_y_sum(alpha: &[f64], y: &[f64]) -> f64 {
        let mut s = 0.0f64;
        for i in 0..alpha.len() {
            s = s + alpha[i] * y[i];
        }
        s
    }

    pub fn alpha_checksum(alpha: &[f64]) -> f64 {
        let mut s = 0.0f64;
        for i in 0..alpha.len() {
            s = s + alpha[i] * (((i % 97) + 1) as f64);
        }
        s
    }
}

// ---------------------------------------------------------------------------
// module app
// ---------------------------------------------------------------------------
mod app {
    use crate::data;
    use crate::svm;

    /// %.6f semantics, with negative zero normalized away.
    fn f6(v: f64) -> String {
        let s = format!("{:.6}", v);
        strip_neg_zero(s)
    }

    fn f12(v: f64) -> String {
        let s = format!("{:.12}", v);
        strip_neg_zero(s)
    }

    fn strip_neg_zero(s: String) -> String {
        if s.starts_with('-') {
            let all_zero = s[1..].chars().all(|c| c == '0' || c == '.');
            if all_zero {
                return s[1..].to_string();
            }
        }
        s
    }

    pub fn run() {
        let ds = data::build();
        let t = svm::train(&ds.x_train, &ds.y_train);
        let m = svm::recover(&ds.x_train, &ds.y_train, &t.alpha);

        // Evaluation.
        let mut train_correct = 0usize;
        for i in 0..ds.x_train.len() {
            let pred = svm::g_sign(&m, &ds.x_train[i]);
            let truth = if ds.y_train[i] > 0.0 { 1 } else { -1 };
            if pred == truth {
                train_correct += 1;
            }
        }

        let mut preds: Vec<i32> = Vec::with_capacity(ds.x_test.len());
        let mut c1_correct = 0usize;
        let mut c2_correct = 0usize;
        for i in 0..ds.x_test.len() {
            let pred = svm::g_sign(&m, &ds.x_test[i]);
            preds.push(pred);
            let truth = if ds.y_test[i] > 0.0 { 1 } else { -1 };
            if pred == truth {
                if i < data::N_TEST_C1 {
                    c1_correct += 1;
                } else {
                    c2_correct += 1;
                }
            }
        }
        let total_correct = c1_correct + c2_correct;

        let train_acc = (train_correct as f64) / (ds.x_train.len() as f64);
        let test_acc = (total_correct as f64) / (ds.x_test.len() as f64);
        let test_acc_c1 = (c1_correct as f64) / (data::N_TEST_C1 as f64);
        let test_acc_c2 = (c2_correct as f64) / (data::N_TEST_C2 as f64);

        let converged: i32 = if t.judge_last { 0 } else { 1 };

        let mut out = String::new();
        out.push_str("SVM_VERSION 1\n");
        out.push_str(&format!("SWEEPS {}\n", svm::SWEEPS));
        out.push_str(&format!("CONVERGED {}\n", converged));
        out.push_str(&format!("ERROR_LAST {}\n", f6(t.error_last)));
        out.push_str(&format!("MAX_ABS_DELTA {}\n", f6(t.max_abs_delta_last)));
        out.push_str(&format!("BETA {}\n", f6(t.beta)));
        out.push_str(&format!("NS_MARGIN {}\n", m.ns_margin));
        out.push_str(&format!("NS_INSIDE {}\n", m.ns_inside));
        out.push_str(&format!(
            "W {} {} {} {}\n",
            f6(m.w[0]),
            f6(m.w[1]),
            f6(m.w[2]),
            f6(m.w[3])
        ));
        out.push_str(&format!("B {}\n", f6(m.b)));
        out.push_str(&format!(
            "OBJECTIVE {}\n",
            f6(svm::objective(&t.alpha, &ds.y_train, &t.gram, t.n))
        ));
        out.push_str(&format!("ALPHA_SUM {}\n", f6(svm::alpha_sum(&t.alpha))));
        out.push_str(&format!(
            "ALPHA_Y_SUM {}\n",
            f12(svm::alpha_y_sum(&t.alpha, &ds.y_train))
        ));
        out.push_str(&format!(
            "ALPHA_CHECKSUM {}\n",
            f6(svm::alpha_checksum(&t.alpha))
        ));
        out.push_str(&format!("TRAIN_ACC {}\n", f6(train_acc)));
        out.push_str(&format!("TEST_ACC {}\n", f6(test_acc)));
        out.push_str(&format!("TEST_ACC_C1 {}\n", f6(test_acc_c1)));
        out.push_str(&format!("TEST_ACC_C2 {}\n", f6(test_acc_c2)));
        out.push_str(&format!(
            "TEST_CORRECT {} {} {}\n",
            c1_correct, c2_correct, total_correct
        ));

        out.push_str("PRED");
        for p in preds.iter() {
            out.push(' ');
            out.push_str(&p.to_string());
        }
        out.push('\n');

        out.push_str("ALPHA");
        for a in t.alpha.iter() {
            out.push(' ');
            out.push_str(&f6(*a));
        }
        out.push('\n');

        print!("{}", out);
    }
}

fn main() {
    app::run();
}
