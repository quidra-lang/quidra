// WL-SVM : SoftMargin-SVM dual ascent, spec PE-P1-svm-specA
// Logical modules: data, svm, app  (see section markers below)
// Standard library only. Single threaded. No linear-algebra / BLAS / stats facility.

const std = @import("std");

// ---------------------------------------------------------------------------
// frozen constants (2.2)
// ---------------------------------------------------------------------------
const D: usize = 4;
const N_TRAIN_C1: usize = 100;
const N_TRAIN_C2: usize = 100;
const N_TEST_C1: usize = 50;
const N_TEST_C2: usize = 50;
const N: usize = N_TRAIN_C1 + N_TRAIN_C2; // 200
const N_TEST: usize = N_TEST_C1 + N_TEST_C2; // 100

const MU1 = [D]f64{ 1.0, 1.0, 0.5, -0.5 };
const MU2 = [D]f64{ -1.0, -1.0, -0.5, 0.5 };
const SIGMA: f64 = 0.8;
const SEED: i64 = 1234567;
const C_BOUND: f64 = 10.0;
const LR: f64 = 0.0001;
const LIMIT: f64 = 0.0001;
const SWEEPS: usize = 1000;
const EPS_SV: f64 = 0.0000001;

const MULT: i64 = 48271;
const MOD: i64 = 2147483647;

// ===========================================================================
// module: data   -- LCG-PM generator and dataset builder
// ===========================================================================

var rng_state: i64 = SEED;

fn nextState() i64 {
    rng_state = @rem(MULT * rng_state, MOD);
    return rng_state;
}

fn nextUniform() f64 {
    const s: f64 = @floatFromInt(nextState());
    return s / 2147483647.0;
}

fn nextNormal() f64 {
    // Irwin-Hall(12) - 6 ; ascending accumulation, not reassociated
    var t: f64 = 0.0;
    var k: usize = 0;
    while (k < 12) : (k += 1) {
        t = t + nextUniform();
    }
    return t - 6.0;
}

var x_train: [N][D]f64 = undefined;
var y_train: [N]f64 = undefined;
var x_test: [N_TEST][D]f64 = undefined;
var y_test: [N_TEST]f64 = undefined;

fn fillBlock(dst: [][D]f64, start: usize, count: usize, mu: [D]f64) void {
    var n: usize = 0;
    while (n < count) : (n += 1) {
        var d: usize = 0;
        while (d < D) : (d += 1) {
            dst[start + n][d] = mu[d] + SIGMA * nextNormal();
        }
    }
}

fn buildDataset() void {
    // exact draw order (2.3)
    fillBlock(x_train[0..], 0, N_TRAIN_C1, MU1);
    fillBlock(x_train[0..], N_TRAIN_C1, N_TRAIN_C2, MU2);
    fillBlock(x_test[0..], 0, N_TEST_C1, MU1);
    fillBlock(x_test[0..], N_TEST_C1, N_TEST_C2, MU2);

    var i: usize = 0;
    while (i < N) : (i += 1) {
        y_train[i] = if (i < N_TRAIN_C1) 1.0 else -1.0;
    }
    i = 0;
    while (i < N_TEST) : (i += 1) {
        y_test[i] = if (i < N_TEST_C1) 1.0 else -1.0;
    }
}

// ===========================================================================
// module: svm
// ===========================================================================

var gram: [N][N]f64 = undefined;
var alpha: [N]f64 = undefined;
var beta: f64 = 1.0;

var last_judge: bool = false;
var last_error: f64 = 0.0;
var last_max_abs_delta: f64 = 0.0;

fn buildGram() void {
    var i: usize = 0;
    while (i < N) : (i += 1) {
        var j: usize = 0;
        while (j < N) : (j += 1) {
            var s: f64 = 0.0;
            var d: usize = 0;
            while (d < D) : (d += 1) {
                s = s + x_train[i][d] * x_train[j][d];
            }
            gram[i][j] = s;
        }
    }
}

fn train() void {
    var i: usize = 0;
    while (i < N) : (i += 1) {
        alpha[i] = 0.0;
    }
    beta = 1.0;

    var sweep: usize = 0;
    while (sweep < SWEEPS) : (sweep += 1) {
        var judge: bool = false;
        var err_acc: f64 = 0.0;
        var max_abs_delta: f64 = 0.0;

        // (3.1) alpha update, ascending i, in place (Gauss-Seidel)
        i = 0;
        while (i < N) : (i += 1) {
            var item1: f64 = 0.0;
            var j: usize = 0;
            while (j < N) : (j += 1) {
                item1 = item1 + alpha[j] * y_train[i] * y_train[j] * gram[i][j];
            }

            var item2: f64 = 0.0;
            j = 0;
            while (j < N) : (j += 1) {
                item2 = item2 + alpha[j] * y_train[i] * y_train[j];
            }

            const delta: f64 = 1.0 - item1 - beta * item2;
            const ad: f64 = @abs(delta);
            if (ad > max_abs_delta) max_abs_delta = ad;

            alpha[i] = alpha[i] + LR * delta;
            if (alpha[i] < 0.0) {
                alpha[i] = 0.0;
            } else if (alpha[i] > C_BOUND) {
                alpha[i] = C_BOUND;
            } else if (ad > LIMIT) {
                judge = true;
                err_acc = err_acc + (ad - LIMIT);
            }
        }

        // (3.2) beta update, once per sweep
        var s: f64 = 0.0;
        i = 0;
        while (i < N) : (i += 1) {
            s = s + alpha[i] * y_train[i];
        }
        beta = beta + s * s / 2.0;

        last_judge = judge;
        last_error = err_acc;
        last_max_abs_delta = max_abs_delta;
    }
}

var s_margin: [N]usize = undefined;
var ns_margin: usize = 0;
var s_inside: [N]usize = undefined;
var ns_inside: usize = 0;
var w: [D]f64 = undefined;
var b_bias: f64 = 0.0;

fn recover() void {
    ns_margin = 0;
    ns_inside = 0;
    var i: usize = 0;
    while (i < N) : (i += 1) {
        if (EPS_SV < alpha[i] and alpha[i] < C_BOUND - EPS_SV) {
            s_margin[ns_margin] = i;
            ns_margin += 1;
        }
        if (alpha[i] >= C_BOUND - EPS_SV) {
            s_inside[ns_inside] = i;
            ns_inside += 1;
        }
    }

    var d: usize = 0;
    while (d < D) : (d += 1) {
        w[d] = 0.0;
    }
    d = 0;
    while (d < D) : (d += 1) {
        var k: usize = 0;
        while (k < ns_margin) : (k += 1) {
            const idx = s_margin[k];
            w[d] = w[d] + alpha[idx] * y_train[idx] * x_train[idx][d];
        }
        k = 0;
        while (k < ns_inside) : (k += 1) {
            const idx = s_inside[k];
            w[d] = w[d] + alpha[idx] * y_train[idx] * x_train[idx][d];
        }
    }

    var bb: f64 = 0.0;
    var k: usize = 0;
    while (k < ns_margin) : (k += 1) {
        const idx = s_margin[k];
        var dp: f64 = 0.0;
        d = 0;
        while (d < D) : (d += 1) {
            dp = dp + w[d] * x_train[idx][d];
        }
        bb = bb + (y_train[idx] - dp);
    }
    const cnt: f64 = @floatFromInt(ns_margin);
    b_bias = bb / cnt;
}

fn fval(p: *const [D]f64) f64 {
    var s: f64 = 0.0;
    var d: usize = 0;
    while (d < D) : (d += 1) {
        s = s + w[d] * p[d];
    }
    return s + b_bias;
}

fn gval(p: *const [D]f64) f64 {
    return if (fval(p) >= 0.0) 1.0 else -1.0;
}

// ===========================================================================
// module: app
// ===========================================================================

var outbuf: [262144]u8 = undefined;
var outlen: usize = 0;

fn emit(comptime fmt: []const u8, args: anytype) void {
    const s = std.fmt.bufPrint(outbuf[outlen..], fmt, args) catch return;
    outlen += s.len;
}

fn nz(v: f64) f64 {
    return if (v == 0.0) 0.0 else v;
}

pub fn main() void {
    buildDataset();
    buildGram();
    train();
    recover();

    // aggregate quantities
    var alpha_sum: f64 = 0.0;
    var i: usize = 0;
    while (i < N) : (i += 1) {
        alpha_sum = alpha_sum + alpha[i];
    }

    var alpha_y_sum: f64 = 0.0;
    i = 0;
    while (i < N) : (i += 1) {
        alpha_y_sum = alpha_y_sum + alpha[i] * y_train[i];
    }

    var quad: f64 = 0.0;
    i = 0;
    while (i < N) : (i += 1) {
        var inner: f64 = 0.0;
        var j: usize = 0;
        while (j < N) : (j += 1) {
            inner = inner + alpha[i] * alpha[j] * y_train[i] * y_train[j] * gram[i][j];
        }
        quad = quad + inner;
    }
    const objective: f64 = alpha_sum - 0.5 * quad;

    var checksum: f64 = 0.0;
    i = 0;
    while (i < N) : (i += 1) {
        const m: f64 = @floatFromInt((i % 97) + 1);
        checksum = checksum + alpha[i] * m;
    }

    var train_correct: usize = 0;
    i = 0;
    while (i < N) : (i += 1) {
        if (gval(&x_train[i]) == y_train[i]) train_correct += 1;
    }

    var pred: [N_TEST]i64 = undefined;
    var correct_c1: usize = 0;
    var correct_c2: usize = 0;
    i = 0;
    while (i < N_TEST) : (i += 1) {
        const g = gval(&x_test[i]);
        pred[i] = if (g >= 0.0) 1 else -1;
        if (g == y_test[i]) {
            if (i < N_TEST_C1) correct_c1 += 1 else correct_c2 += 1;
        }
    }
    const total_correct: usize = correct_c1 + correct_c2;

    const train_acc: f64 = @as(f64, @floatFromInt(train_correct)) / @as(f64, @floatFromInt(N));
    const test_acc: f64 = @as(f64, @floatFromInt(total_correct)) / @as(f64, @floatFromInt(N_TEST));
    const test_acc_c1: f64 = @as(f64, @floatFromInt(correct_c1)) / @as(f64, @floatFromInt(N_TEST_C1));
    const test_acc_c2: f64 = @as(f64, @floatFromInt(correct_c2)) / @as(f64, @floatFromInt(N_TEST_C2));

    const converged: i64 = if (last_judge) 0 else 1;

    emit("SVM_VERSION 1\n", .{});
    emit("SWEEPS {d}\n", .{SWEEPS});
    emit("CONVERGED {d}\n", .{converged});
    emit("ERROR_LAST {d:.6}\n", .{nz(last_error)});
    emit("MAX_ABS_DELTA {d:.6}\n", .{nz(last_max_abs_delta)});
    emit("BETA {d:.6}\n", .{nz(beta)});
    emit("NS_MARGIN {d}\n", .{ns_margin});
    emit("NS_INSIDE {d}\n", .{ns_inside});

    emit("W", .{});
    var d: usize = 0;
    while (d < D) : (d += 1) {
        emit(" {d:.6}", .{nz(w[d])});
    }
    emit("\n", .{});

    emit("B {d:.6}\n", .{nz(b_bias)});
    emit("OBJECTIVE {d:.6}\n", .{nz(objective)});
    emit("ALPHA_SUM {d:.6}\n", .{nz(alpha_sum)});
    emit("ALPHA_Y_SUM {d:.12}\n", .{nz(alpha_y_sum)});
    emit("ALPHA_CHECKSUM {d:.6}\n", .{nz(checksum)});
    emit("TRAIN_ACC {d:.6}\n", .{nz(train_acc)});
    emit("TEST_ACC {d:.6}\n", .{nz(test_acc)});
    emit("TEST_ACC_C1 {d:.6}\n", .{nz(test_acc_c1)});
    emit("TEST_ACC_C2 {d:.6}\n", .{nz(test_acc_c2)});
    emit("TEST_CORRECT {d} {d} {d}\n", .{ correct_c1, correct_c2, total_correct });

    emit("PRED", .{});
    i = 0;
    while (i < N_TEST) : (i += 1) {
        emit(" {d}", .{pred[i]});
    }
    emit("\n", .{});

    emit("ALPHA", .{});
    i = 0;
    while (i < N) : (i += 1) {
        emit(" {d:.6}", .{nz(alpha[i])});
    }
    emit("\n", .{});

    const io = std.Options.debug_io;
    std.Io.File.stdout().writeStreamingAll(io, outbuf[0..outlen]) catch {};
}
