// WL-SVM / PE-P1-svm-specA -- Swift implementation
// Standard library only. No linear-algebra / tensor / statistics / BLAS facility used.
// Single threaded, no SIMD, no intrinsics.

import Foundation

// ===========================================================================
// MARK: - Frozen constants
// ===========================================================================

enum Const {
    static let D = 4
    static let N_TRAIN_C1 = 100
    static let N_TRAIN_C2 = 100
    static let N_TEST_C1 = 50
    static let N_TEST_C2 = 50
    static let MU1: [Double] = [1.0, 1.0, 0.5, -0.5]
    static let MU2: [Double] = [-1.0, -1.0, -0.5, 0.5]
    static let SIGMA = 0.8
    static let SEED = 1234567
    static let C = 10.0
    static let LR = 0.0001
    static let LIMIT = 0.0001
    static let SWEEPS = 1000
    static let EPS_SV = 0.0000001
}

// ===========================================================================
// MARK: - module `data` : LCG-PM generator and dataset builder
// ===========================================================================

enum Data {

    /// Lehmer / Park-Miller minimal standard multiplicative generator.
    /// state_{n+1} = (48271 * state_n) mod 2147483647
    struct LCGPM {
        static let MULT = 48271
        static let MOD = 2147483647
        private var state: Int

        init(seed: Int) {
            self.state = seed
        }

        mutating func nextState() -> Int {
            state = (LCGPM.MULT * state) % LCGPM.MOD
            return state
        }

        mutating func nextUniform() -> Double {
            return Double(nextState()) / 2147483647.0
        }

        /// Irwin-Hall(12) - 6, ascending accumulation, never reassociated.
        mutating func nextNormal() -> Double {
            var t = 0.0
            for _ in 0..<12 {
                t = t + nextUniform()
            }
            return t - 6.0
        }
    }

    /// The complete dataset, flattened row-major (`D` doubles per point).
    struct Dataset {
        var xTrain: [Double]   // (N_TRAIN_C1 + N_TRAIN_C2) * D
        var yTrain: [Double]   // +1 for i < N_TRAIN_C1, -1 afterwards
        var xTestC1: [Double]  // N_TEST_C1 * D
        var xTestC2: [Double]  // N_TEST_C2 * D
        var n: Int
    }

    /// Draws one block of `count` points around `mu`, points ascending,
    /// dimensions ascending, from the single shared stream.
    private static func drawBlock(_ rng: inout LCGPM, _ mu: [Double], _ count: Int) -> [Double] {
        var out = [Double](repeating: 0.0, count: count * Const.D)
        for n in 0..<count {
            for d in 0..<Const.D {
                out[n * Const.D + d] = mu[d] + Const.SIGMA * rng.nextNormal()
            }
        }
        return out
    }

    /// Exact draw order of the specification: train +1, train -1, test +1, test -1.
    static func build() -> Dataset {
        var rng = LCGPM(seed: Const.SEED)

        let trainC1 = drawBlock(&rng, Const.MU1, Const.N_TRAIN_C1)
        let trainC2 = drawBlock(&rng, Const.MU2, Const.N_TRAIN_C2)
        let testC1 = drawBlock(&rng, Const.MU1, Const.N_TEST_C1)
        let testC2 = drawBlock(&rng, Const.MU2, Const.N_TEST_C2)

        // Concatenate class +1 first, class -1 second.
        var xTrain = [Double]()
        xTrain.reserveCapacity(trainC1.count + trainC2.count)
        xTrain.append(contentsOf: trainC1)
        xTrain.append(contentsOf: trainC2)

        var yTrain = [Double](repeating: 1.0, count: Const.N_TRAIN_C1 + Const.N_TRAIN_C2)
        for i in Const.N_TRAIN_C1..<(Const.N_TRAIN_C1 + Const.N_TRAIN_C2) {
            yTrain[i] = -1.0
        }

        return Dataset(xTrain: xTrain,
                       yTrain: yTrain,
                       xTestC1: testC1,
                       xTestC2: testC2,
                       n: Const.N_TRAIN_C1 + Const.N_TRAIN_C2)
    }
}

// ===========================================================================
// MARK: - module `svm` : training, SV extraction, w/b recovery, f, g, eval
// ===========================================================================

enum SVM {

    struct TrainResult {
        var alpha: [Double]
        var beta: Double
        var judge: Bool
        var errorLast: Double
        var maxAbsDelta: Double
        var gram: [Double]
    }

    struct Model {
        var w: [Double]
        var b: Double
        var nsMargin: Int
        var nsInside: Int
    }

    /// Mandatory one-off Gram build: G[i][j] = sum_d x[i][d] * x[j][d].
    static func buildGram(_ x: [Double], _ n: Int) -> [Double] {
        var g = [Double](repeating: 0.0, count: n * n)
        for i in 0..<n {
            for j in 0..<n {
                var s = 0.0
                for d in 0..<Const.D {
                    s = s + x[i * Const.D + d] * x[j * Const.D + d]
                }
                g[i * n + j] = s
            }
        }
        return g
    }

    /// Exactly SWEEPS sweeps of in-place (Gauss-Seidel) dual ascent, no early exit.
    static func train(_ x: [Double], _ y: [Double], _ n: Int) -> TrainResult {
        let g = buildGram(x, n)

        var alpha = [Double](repeating: 0.0, count: n)
        var beta = 1.0

        var judge = false
        var error = 0.0
        var maxAbsDelta = 0.0

        for _ in 0..<Const.SWEEPS {

            judge = false
            error = 0.0
            maxAbsDelta = 0.0

            // (3.1) update alpha, ascending i, in place
            for i in 0..<n {
                let yi = y[i]
                let rowBase = i * n

                var item1 = 0.0
                for j in 0..<n {
                    item1 = item1 + alpha[j] * yi * y[j] * g[rowBase + j]
                }

                var item2 = 0.0
                for j in 0..<n {
                    item2 = item2 + alpha[j] * yi * y[j]
                }

                let delta = 1.0 - item1 - beta * item2
                let ad = abs(delta)

                if ad > maxAbsDelta { maxAbsDelta = ad }

                alpha[i] = alpha[i] + Const.LR * delta
                if alpha[i] < 0.0 {
                    alpha[i] = 0.0
                } else if alpha[i] > Const.C {
                    alpha[i] = Const.C
                } else if ad > Const.LIMIT {
                    judge = true
                    error = error + (ad - Const.LIMIT)
                }
            }

            // (3.2) update beta, once per sweep
            var s = 0.0
            for i in 0..<n {
                s = s + alpha[i] * y[i]
            }
            beta = beta + s * s / 2.0
        }

        return TrainResult(alpha: alpha,
                           beta: beta,
                           judge: judge,
                           errorLast: error,
                           maxAbsDelta: maxAbsDelta,
                           gram: g)
    }

    /// Support-vector extraction plus w / b recovery.
    static func recover(_ alpha: [Double], _ x: [Double], _ y: [Double], _ n: Int) -> Model {
        var sMargin = [Int]()
        var sInside = [Int]()
        for i in 0..<n {
            if Const.EPS_SV < alpha[i] && alpha[i] < Const.C - Const.EPS_SV {
                sMargin.append(i)
            }
            if alpha[i] >= Const.C - Const.EPS_SV {
                sInside.append(i)
            }
        }

        var w = [Double](repeating: 0.0, count: Const.D)
        for d in 0..<Const.D {
            for i in sMargin {
                w[d] = w[d] + alpha[i] * y[i] * x[i * Const.D + d]
            }
            for i in sInside {
                w[d] = w[d] + alpha[i] * y[i] * x[i * Const.D + d]
            }
        }

        var b = 0.0
        for i in sMargin {
            var dp = 0.0
            for d in 0..<Const.D {
                dp = dp + w[d] * x[i * Const.D + d]
            }
            b = b + (y[i] - dp)
        }
        b = b / Double(sMargin.count)

        return Model(w: w, b: b, nsMargin: sMargin.count, nsInside: sInside.count)
    }

    static func f(_ model: Model, _ p: [Double], _ off: Int) -> Double {
        var s = 0.0
        for d in 0..<Const.D {
            s = s + model.w[d] * p[off + d]
        }
        return s + model.b
    }

    static func g(_ model: Model, _ p: [Double], _ off: Int) -> Int {
        return f(model, p, off) >= 0.0 ? 1 : -1
    }

    /// Counts correct predictions over a block whose true label is `label`.
    static func countCorrect(_ model: Model, _ block: [Double], _ count: Int, _ label: Int) -> Int {
        var c = 0
        for i in 0..<count {
            if g(model, block, i * Const.D) == label { c += 1 }
        }
        return c
    }

    static func objective(_ alpha: [Double], _ y: [Double], _ gram: [Double], _ n: Int) -> Double {
        var sumAlpha = 0.0
        for i in 0..<n {
            sumAlpha = sumAlpha + alpha[i]
        }
        var outer = 0.0
        for i in 0..<n {
            var inner = 0.0
            for j in 0..<n {
                inner = inner + alpha[i] * alpha[j] * y[i] * y[j] * gram[i * n + j]
            }
            outer = outer + inner
        }
        return sumAlpha - 0.5 * outer
    }
}

// ===========================================================================
// MARK: - module `app` : entry point and output formatting
// ===========================================================================

enum App {

    /// Fixed-point, exactly 6 fractional digits, negative zero normalized away.
    static func f6(_ v: Double) -> String {
        let x = (v == 0.0) ? 0.0 : v
        return String(format: "%.6f", x)
    }

    /// Fixed-point, exactly 12 fractional digits, negative zero normalized away.
    static func f12(_ v: Double) -> String {
        let x = (v == 0.0) ? 0.0 : v
        return String(format: "%.12f", x)
    }

    static func run() {
        let ds = Data.build()
        let n = ds.n

        let tr = SVM.train(ds.xTrain, ds.yTrain, n)
        let model = SVM.recover(tr.alpha, ds.xTrain, ds.yTrain, n)

        // Dual sums
        var alphaSum = 0.0
        for i in 0..<n { alphaSum = alphaSum + tr.alpha[i] }

        var alphaYSum = 0.0
        for i in 0..<n { alphaYSum = alphaYSum + tr.alpha[i] * ds.yTrain[i] }

        var alphaChecksum = 0.0
        for i in 0..<n {
            alphaChecksum = alphaChecksum + tr.alpha[i] * Double((i % 97) + 1)
        }

        let obj = SVM.objective(tr.alpha, ds.yTrain, tr.gram, n)

        // Training accuracy over the 200 training points, +1 block then -1 block.
        var trainCorrect = 0
        for i in 0..<n {
            let pred = SVM.g(model, ds.xTrain, i * Const.D)
            let truth = ds.yTrain[i] > 0.0 ? 1 : -1
            if pred == truth { trainCorrect += 1 }
        }
        let trainAcc = Double(trainCorrect) / Double(n)

        // Test predictions and accuracies.
        var preds = [Int]()
        preds.reserveCapacity(Const.N_TEST_C1 + Const.N_TEST_C2)
        for i in 0..<Const.N_TEST_C1 {
            preds.append(SVM.g(model, ds.xTestC1, i * Const.D))
        }
        for i in 0..<Const.N_TEST_C2 {
            preds.append(SVM.g(model, ds.xTestC2, i * Const.D))
        }

        let correctC1 = SVM.countCorrect(model, ds.xTestC1, Const.N_TEST_C1, 1)
        let correctC2 = SVM.countCorrect(model, ds.xTestC2, Const.N_TEST_C2, -1)
        let correctTotal = correctC1 + correctC2

        let testAcc = Double(correctTotal) / Double(Const.N_TEST_C1 + Const.N_TEST_C2)
        let testAccC1 = Double(correctC1) / Double(Const.N_TEST_C1)
        let testAccC2 = Double(correctC2) / Double(Const.N_TEST_C2)

        var out = ""
        out += "SVM_VERSION 1\n"
        out += "SWEEPS \(Const.SWEEPS)\n"
        out += "CONVERGED \(tr.judge ? 0 : 1)\n"
        out += "ERROR_LAST \(f6(tr.errorLast))\n"
        out += "MAX_ABS_DELTA \(f6(tr.maxAbsDelta))\n"
        out += "BETA \(f6(tr.beta))\n"
        out += "NS_MARGIN \(model.nsMargin)\n"
        out += "NS_INSIDE \(model.nsInside)\n"
        out += "W " + model.w.map { f6($0) }.joined(separator: " ") + "\n"
        out += "B \(f6(model.b))\n"
        out += "OBJECTIVE \(f6(obj))\n"
        out += "ALPHA_SUM \(f6(alphaSum))\n"
        out += "ALPHA_Y_SUM \(f12(alphaYSum))\n"
        out += "ALPHA_CHECKSUM \(f6(alphaChecksum))\n"
        out += "TRAIN_ACC \(f6(trainAcc))\n"
        out += "TEST_ACC \(f6(testAcc))\n"
        out += "TEST_ACC_C1 \(f6(testAccC1))\n"
        out += "TEST_ACC_C2 \(f6(testAccC2))\n"
        out += "TEST_CORRECT \(correctC1) \(correctC2) \(correctTotal)\n"
        out += "PRED " + preds.map { String($0) }.joined(separator: " ") + "\n"
        out += "ALPHA " + tr.alpha.map { f6($0) }.joined(separator: " ") + "\n"

        print(out, terminator: "")
    }
}

App.run()
