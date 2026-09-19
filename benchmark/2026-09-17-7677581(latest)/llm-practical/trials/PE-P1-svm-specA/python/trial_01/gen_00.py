#!/usr/bin/env python3
"""WL-SVM / PE-P1-svm-specA -- SoftMargin-SVM from the frozen specification.

Standard library only.  No linear-algebra / tensor / statistics / BLAS facility is
used (no numpy, no scipy, no statistics, no math.fsum): every accumulation below is
a plain scalar fold in the pinned ascending index order.

Three logical modules, as required by 2.1:
    data  -- the LCG-PM generator and the dataset builder
    svm   -- training, support-vector extraction, w/b recovery, f, g, evaluation
    app   -- entry point and output formatting
"""

from operator import mul

# ----------------------------------------------------------------------------
# Frozen constants (2.2)
# ----------------------------------------------------------------------------
D = 4
N_TRAIN_C1 = 100
N_TRAIN_C2 = 100
N_TEST_C1 = 50
N_TEST_C2 = 50
MU1 = [1.0, 1.0, 0.5, -0.5]
MU2 = [-1.0, -1.0, -0.5, 0.5]
SIGMA = 0.8
SEED = 1234567
C = 10.0
LR = 0.0001
LIMIT = 0.0001
SWEEPS = 1000
EPS_SV = 0.0000001

N = N_TRAIN_C1 + N_TRAIN_C2

# LCG-PM parameters (1.2)
MULT = 48271
MOD = 2147483647


# ============================================================================
# module: data
# ============================================================================
class data(object):
    """LCG-PM generator and dataset builder."""

    class LcgPm(object):
        """Lehmer / Park-Miller minimal standard generator."""

        __slots__ = ("state",)

        def __init__(self, seed):
            self.state = seed

        def next_state(self):
            self.state = (MULT * self.state) % MOD
            return self.state

        def next_uniform(self):
            self.state = (MULT * self.state) % MOD
            return self.state / 2147483647.0

        def next_normal(self):
            # Irwin-Hall(12) - 6 ; ascending accumulation, never reassociated.
            t = 0.0
            nu = self.next_uniform
            for _ in range(12):
                t = t + nu()
            return t - 6.0

    @staticmethod
    def draw_block(rng, mu, n):
        """n points, points ascending, dimensions ascending."""
        block = []
        for _ in range(n):
            p = []
            for d in range(D):
                p.append(mu[d] + SIGMA * rng.next_normal())
            block.append(p)
        return block

    @staticmethod
    def build():
        """One stream, seeded once, drawn strictly sequentially (2.3)."""
        rng = data.LcgPm(SEED)
        train_c1 = data.draw_block(rng, MU1, N_TRAIN_C1)
        train_c2 = data.draw_block(rng, MU2, N_TRAIN_C2)
        test_c1 = data.draw_block(rng, MU1, N_TEST_C1)
        test_c2 = data.draw_block(rng, MU2, N_TEST_C2)

        x = train_c1 + train_c2
        y = [1.0] * N_TRAIN_C1 + [-1.0] * N_TRAIN_C2
        return x, y, test_c1, test_c2


# ============================================================================
# module: svm
# ============================================================================
class svm(object):
    """Training, support-vector extraction, w/b recovery, f, g, evaluation."""

    @staticmethod
    def gram(x):
        """G[i][j] = sum over d ascending of x[i][d]*x[j][d].  Mandatory (2.4)."""
        n = len(x)
        g = []
        for i in range(n):
            xi = x[i]
            row = []
            for j in range(n):
                xj = x[j]
                s = 0.0
                for d in range(D):
                    s = s + xi[d] * xj[d]
                row.append(s)
            g.append(row)
        return g

    @staticmethod
    def train(g, y):
        """Exactly SWEEPS sweeps of dual ascent, no early exit (2.4)."""
        n = len(y)

        # Sign-folded Gram rows: gs[i][j] = y[i]*y[j]*G[i][j].
        # Permitted rewrite (2) -- multiplication by +-1 is exact in IEEE-754, so
        # alpha[j]*gs[i][j] is bitwise identical to alpha[j]*y[i]*y[j]*G[i][j].
        gs = []
        for i in range(n):
            yi = y[i]
            gi = g[i]
            gs.append([yi * y[j] * gi[j] for j in range(n)])

        alpha = [0.0] * n
        beta = 1.0

        judge = False
        error = 0.0
        max_abs_delta = 0.0

        for _sweep in range(SWEEPS):

            judge = False
            error = 0.0
            max_abs_delta = 0.0

            # (3.1) update alpha, ascending i, in place (Gauss-Seidel)
            for i in range(n):
                item1 = sum(map(mul, alpha, gs[i]))

                # Permitted rewrite (1): item2 = y[i] * sum_j alpha[j]*y[j],
                # recomputed for every i in ascending j order.
                item2 = y[i] * sum(map(mul, alpha, y))

                delta = 1.0 - item1 - beta * item2

                ad = delta if delta >= 0.0 else -delta
                if ad > max_abs_delta:
                    max_abs_delta = ad

                a = alpha[i] + LR * delta
                if a < 0.0:
                    a = 0.0
                elif a > C:
                    a = C
                elif ad > LIMIT:
                    judge = True
                    error = error + (ad - LIMIT)
                alpha[i] = a

            # (3.2) update beta, once per sweep
            s = 0.0
            for i in range(n):
                s = s + alpha[i] * y[i]
            beta = beta + s * s / 2.0

        return alpha, beta, judge, error, max_abs_delta

    @staticmethod
    def support_sets(alpha):
        s_margin = []
        s_inside = []
        for i in range(len(alpha)):
            if EPS_SV < alpha[i] and alpha[i] < C - EPS_SV:
                s_margin.append(i)
            if alpha[i] >= C - EPS_SV:
                s_inside.append(i)
        return s_margin, s_inside

    @staticmethod
    def recover(alpha, y, x, s_margin, s_inside):
        w = [0.0] * D
        for d in range(D):
            acc = w[d]
            for i in s_margin:
                acc = acc + alpha[i] * y[i] * x[i][d]
            for i in s_inside:
                acc = acc + alpha[i] * y[i] * x[i][d]
            w[d] = acc

        b = 0.0
        for i in s_margin:
            dp = 0.0
            xi = x[i]
            for d in range(D):
                dp = dp + w[d] * xi[d]
            b = b + (y[i] - dp)
        b = b / float(len(s_margin))
        return w, b

    @staticmethod
    def f(w, b, p):
        s = 0.0
        for d in range(D):
            s = s + w[d] * p[d]
        return s + b

    @staticmethod
    def g(w, b, p):
        return 1 if svm.f(w, b, p) >= 0.0 else -1

    @staticmethod
    def count_correct(w, b, points, label):
        c = 0
        for p in points:
            if svm.g(w, b, p) == label:
                c += 1
        return c

    @staticmethod
    def objective(alpha, y, g_mat):
        n = len(alpha)
        s1 = 0.0
        for i in range(n):
            s1 = s1 + alpha[i]
        s2 = 0.0
        for i in range(n):
            inner = 0.0
            ai = alpha[i]
            yi = y[i]
            gi = g_mat[i]
            for j in range(n):
                inner = inner + ai * alpha[j] * yi * y[j] * gi[j]
            s2 = s2 + inner
        return s1 - 0.5 * s2

    @staticmethod
    def alpha_sum(alpha):
        s = 0.0
        for a in alpha:
            s = s + a
        return s

    @staticmethod
    def alpha_y_sum(alpha, y):
        s = 0.0
        for i in range(len(alpha)):
            s = s + alpha[i] * y[i]
        return s

    @staticmethod
    def alpha_checksum(alpha):
        s = 0.0
        for i in range(len(alpha)):
            s = s + alpha[i] * float((i % 97) + 1)
        return s


# ============================================================================
# module: app
# ============================================================================
class app(object):
    """Entry point and output formatting."""

    @staticmethod
    def f6(v):
        if v == 0.0:
            v = 0.0
        t = "%.6f" % v
        if t == "-0.000000":
            t = "0.000000"
        return t

    @staticmethod
    def f12(v):
        if v == 0.0:
            v = 0.0
        t = "%.12f" % v
        if t == "-0.000000000000":
            t = "0.000000000000"
        return t

    @staticmethod
    def main():
        x, y, test_c1, test_c2 = data.build()

        g_mat = svm.gram(x)
        alpha, beta, judge, error, max_abs_delta = svm.train(g_mat, y)

        s_margin, s_inside = svm.support_sets(alpha)
        w, b = svm.recover(alpha, y, x, s_margin, s_inside)

        # training accuracy: class +1 block then class -1 block
        train_correct = 0
        for i in range(N):
            label = 1 if y[i] > 0.0 else -1
            if svm.g(w, b, x[i]) == label:
                train_correct += 1
        train_acc = float(train_correct) / float(N)

        c1_correct = svm.count_correct(w, b, test_c1, 1)
        c2_correct = svm.count_correct(w, b, test_c2, -1)
        test_total = c1_correct + c2_correct
        n_test = N_TEST_C1 + N_TEST_C2

        test_acc = float(test_total) / float(n_test)
        test_acc_c1 = float(c1_correct) / float(N_TEST_C1)
        test_acc_c2 = float(c2_correct) / float(N_TEST_C2)

        obj = svm.objective(alpha, y, g_mat)

        out = []
        out.append("SVM_VERSION 1")
        out.append("SWEEPS %d" % SWEEPS)
        out.append("CONVERGED %d" % (0 if judge else 1))
        out.append("ERROR_LAST " + app.f6(error))
        out.append("MAX_ABS_DELTA " + app.f6(max_abs_delta))
        out.append("BETA " + app.f6(beta))
        out.append("NS_MARGIN %d" % len(s_margin))
        out.append("NS_INSIDE %d" % len(s_inside))
        out.append("W " + " ".join(app.f6(v) for v in w))
        out.append("B " + app.f6(b))
        out.append("OBJECTIVE " + app.f6(obj))
        out.append("ALPHA_SUM " + app.f6(svm.alpha_sum(alpha)))
        out.append("ALPHA_Y_SUM " + app.f12(svm.alpha_y_sum(alpha, y)))
        out.append("ALPHA_CHECKSUM " + app.f6(svm.alpha_checksum(alpha)))
        out.append("TRAIN_ACC " + app.f6(train_acc))
        out.append("TEST_ACC " + app.f6(test_acc))
        out.append("TEST_ACC_C1 " + app.f6(test_acc_c1))
        out.append("TEST_ACC_C2 " + app.f6(test_acc_c2))
        out.append("TEST_CORRECT %d %d %d" % (c1_correct, c2_correct, test_total))

        preds = []
        for p in test_c1:
            preds.append(svm.g(w, b, p))
        for p in test_c2:
            preds.append(svm.g(w, b, p))
        out.append("PRED " + " ".join("%d" % v for v in preds))

        out.append("ALPHA " + " ".join(app.f6(a) for a in alpha))

        import sys
        sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    app.main()
