#!/usr/bin/env python3
"""WL-SVM reference implementation (Python).

Reference oracle for workload WL-SVM of benchmark run 2026-09-17-7677581.
Implements methodology/07_algorithm_workloads.md section 2 literally.

This file is measurement infrastructure, not a benchmark submission. It is
written for obvious correctness, not for speed. Every accumulation is
performed in the index order pinned by the specification; nothing is
reassociated, vectorized, or compensated.

Logical modules required by 07 section 2.1: `data`, `svm`, `app`.
They are spelled here as classes used as namespaces, which 07 section 1.9
permits explicitly.

Standard library only; no numpy.
"""

# =============================================================================
# Frozen constants (07 section 2.2)
# =============================================================================

D = 4  # feature dimension
N_TRAIN_C1 = 100  # class +1 training points
N_TRAIN_C2 = 100  # class -1 training points
N_TEST_C1 = 50  # class +1 test points
N_TEST_C2 = 50  # class -1 test points
MU1 = [1.0, 1.0, 0.5, -0.5]  # class +1 mean
MU2 = [-1.0, -1.0, -0.5, 0.5]  # class -1 mean
SIGMA = 0.8  # per-dimension scale, both classes
SEED = 1234567  # LCG-PM seed
C = 10.0  # regularization bound
LR = 0.0001  # dual ascent step
LIMIT = 0.0001  # KKT residual threshold
SWEEPS = 1000  # fixed number of full sweeps, no early exit
EPS_SV = 0.0000001  # support-vector membership epsilon

N = N_TRAIN_C1 + N_TRAIN_C2  # total training size = 200


# =============================================================================
# Module `data` -- the LCG-PM generator and the dataset builder (07 sections
# 1.2 and 2.3).
# =============================================================================

class data:
    """The frozen deterministic RNG and the dataset builder."""

    MULT = 48271
    MOD = 2147483647  # 2**31 - 1, prime

    class LCGPM:
        """Lehmer / Park-Miller 'minimal standard' multiplicative generator.

        state_{n+1} = (48271 * state_n) mod 2147483647

        Exactly one stream exists per workload. It is seeded once, drawn
        strictly sequentially, never reset and never forked.
        """

        def __init__(self, seed):
            self.state = seed

        def next_state(self):
            # Exact integer arithmetic. The largest intermediate is
            # 48271 * 2147483646 ~= 1.0369e14, well inside 2**53, so this is
            # exact in every language of the comparison set.
            self.state = (data.MULT * self.state) % data.MOD
            return self.state

        def next_uniform(self):
            """A double in (0, 1)."""
            return float(self.next_state()) / 2147483647.0

        def next_normal(self):
            """Irwin-Hall(12) - 6 approximate standard normal.

            No log/sqrt/sin/cos, so the generated dataset is bit-identical in
            all ten languages. The accumulation order is ascending and must
            not be reassociated.
            """
            t = 0.0
            for _ in range(12):
                t = t + self.next_uniform()
            return t - 6.0

    @staticmethod
    def make_block(rng, count, mu):
        """Draw `count` points around mean `mu`.

        Points ascending; within a point, dimensions ascending.
        """
        block = []
        for _n in range(count):
            point = []
            for d in range(D):
                point.append(mu[d] + SIGMA * rng.next_normal())
            block.append(point)
        return block

    @staticmethod
    def build():
        """Generate the four blocks in the exact pinned draw order.

        Returns (x_train, y_train, test_c1, test_c2).
        """
        rng = data.LCGPM(SEED)

        # 1. train class +1
        train_c1 = data.make_block(rng, N_TRAIN_C1, MU1)
        # 2. train class -1
        train_c2 = data.make_block(rng, N_TRAIN_C2, MU2)
        # 3. test class +1
        test_c1 = data.make_block(rng, N_TEST_C1, MU1)
        # 4. test class -1
        test_c2 = data.make_block(rng, N_TEST_C2, MU2)

        # Concatenate the training arrays: class +1 first, class -1 second.
        # This is the reference's (1.1)-then-(1.2) ordering.
        x_train = []
        y_train = []
        for point in train_c1:
            x_train.append(point)
            y_train.append(1)
        for point in train_c2:
            x_train.append(point)
            y_train.append(-1)

        return x_train, y_train, test_c1, test_c2


# =============================================================================
# Module `svm` -- training, support-vector extraction, w/b recovery, f, g,
# evaluation (07 sections 2.4 and 2.5).
# =============================================================================

class svm:
    """The soft margin SVM model."""

    def __init__(self):
        self.x = None
        self.y = None
        self.alpha = None
        self.beta = 1.0
        self.w = [0.0] * D
        self.b = 0.0
        self.s_margin = []
        self.s_inside = []
        # Diagnostics from the final sweep.
        self.judge = False
        self.error = 0.0
        self.max_abs_delta = 0.0

    # -- Gram matrix ------------------------------------------------------

    @staticmethod
    def gram(x):
        """G[i][j] = sum over d ascending of x[i][d] * x[j][d].

        Precomputing G is mandatory, not optional. G[i][j] is bit-identical
        to the reference's per-iteration dot(x[i], x[j]).
        """
        n = len(x)
        g = []
        for i in range(n):
            row = []
            for j in range(n):
                acc = 0.0
                for d in range(D):
                    acc = acc + x[i][d] * x[j][d]
                row.append(acc)
            g.append(row)
        return g

    # -- Training ---------------------------------------------------------

    def train(self, x, y):
        """Run exactly SWEEPS full sweeps of dual coordinate ascent."""
        self.x = x
        self.y = y
        n = len(x)

        g = svm.gram(x)

        alpha = [0.0] * n
        beta = 1.0

        for _sweep in range(SWEEPS):

            judge = False
            error = 0.0
            max_abs_delta = 0.0

            # (3.1) Update alpha, in ascending i, in place. alpha[j] for
            # j < i is already updated (Gauss-Seidel, not Jacobi).
            for i in range(n):

                yi = float(y[i])
                g_i = g[i]

                item1 = 0.0
                for j in range(n):
                    item1 = item1 + alpha[j] * yi * float(y[j]) * g_i[j]

                item2 = 0.0
                for j in range(n):
                    item2 = item2 + alpha[j] * yi * float(y[j])

                delta = 1.0 - item1 - beta * item2

                if abs(delta) > max_abs_delta:
                    max_abs_delta = abs(delta)

                # The if / elif / elif chain is exactly the reference's.
                # When alpha[i] is clamped to 0.0 or to C the convergence
                # flag is NOT set and `error` is NOT accumulated, even if
                # |delta| > LIMIT. This is deliberate; do not "fix" it.
                alpha[i] = alpha[i] + LR * delta
                if alpha[i] < 0.0:
                    alpha[i] = 0.0
                elif alpha[i] > C:
                    alpha[i] = C
                elif abs(delta) > LIMIT:
                    judge = True
                    error = error + (abs(delta) - LIMIT)

            # (3.2) Update beta, once per sweep, after the whole i loop.
            s = 0.0
            for i in range(n):
                s = s + alpha[i] * float(y[i])
            beta = beta + s * s / 2.0

        self.alpha = alpha
        self.beta = beta
        self.judge = judge
        self.error = error
        self.max_abs_delta = max_abs_delta
        self.gram_matrix = g

    # -- Model recovery ---------------------------------------------------

    def recover(self):
        """Extract support vectors and recover w and b."""
        alpha = self.alpha
        x = self.x
        y = self.y
        n = len(x)

        self.s_margin = [i for i in range(n)
                         if EPS_SV < alpha[i] and alpha[i] < C - EPS_SV]
        self.s_inside = [i for i in range(n) if alpha[i] >= C - EPS_SV]

        self.w = [0.0] * D
        for d in range(D):
            for i in self.s_margin:
                self.w[d] = self.w[d] + alpha[i] * float(y[i]) * x[i][d]
            for i in self.s_inside:
                self.w[d] = self.w[d] + alpha[i] * float(y[i]) * x[i][d]

        # |S_margin| == 0 would be a FAIL; the frozen run gives 65.
        if len(self.s_margin) == 0:
            raise RuntimeError("no support vectors on margin")

        b = 0.0
        for i in self.s_margin:
            dp = 0.0
            for d in range(D):
                dp = dp + self.w[d] * x[i][d]
            b = b + (float(y[i]) - dp)
        self.b = b / float(len(self.s_margin))

    # -- Decision functions -----------------------------------------------

    def f(self, p):
        acc = 0.0
        for d in range(D):
            acc = acc + self.w[d] * p[d]
        return acc + self.b

    def g_of(self, p):
        return 1 if self.f(p) >= 0.0 else -1

    # -- Reported aggregates ----------------------------------------------

    def objective(self):
        """Dual objective.

        sum_i alpha[i]
          - 0.5 * sum_i ( sum_j alpha[i]*alpha[j]*y[i]*y[j]*G[i][j] )
        """
        alpha = self.alpha
        y = self.y
        g = self.gram_matrix
        n = len(alpha)

        linear = 0.0
        for i in range(n):
            linear = linear + alpha[i]

        quad = 0.0
        for i in range(n):
            inner = 0.0
            for j in range(n):
                inner = inner + (alpha[i] * alpha[j] * float(y[i])
                                 * float(y[j]) * g[i][j])
            quad = quad + inner

        return linear - 0.5 * quad

    def alpha_sum(self):
        acc = 0.0
        for a in self.alpha:
            acc = acc + a
        return acc

    def alpha_y_sum(self):
        acc = 0.0
        for i in range(len(self.alpha)):
            acc = acc + self.alpha[i] * float(self.y[i])
        return acc

    def alpha_checksum(self):
        """Ordering-sensitive checksum: sum_i alpha[i] * ((i mod 97) + 1)."""
        acc = 0.0
        for i in range(len(self.alpha)):
            acc = acc + self.alpha[i] * float((i % 97) + 1)
        return acc


# =============================================================================
# Module `app` -- entry point and output formatting (07 sections 1.4 and 2.7).
# =============================================================================

class app:
    """Entry point and output formatting."""

    @staticmethod
    def fixed(value, digits):
        """Format as fixed-point with exactly `digits` decimals.

        Negative zero is forbidden in output, so a value that renders as all
        zero digits is emitted without a sign.
        """
        if value == 0.0:
            value = 0.0  # normalizes -0.0 to +0.0
        text = "%.*f" % (digits, value)
        if text.startswith("-") and float(text) == 0.0:
            text = text[1:]
        return text

    @staticmethod
    def f6(value):
        return app.fixed(value, 6)

    @staticmethod
    def f12(value):
        return app.fixed(value, 12)

    @staticmethod
    def main():
        lines = []

        x_train, y_train, test_c1, test_c2 = data.build()

        model = svm()
        model.train(x_train, y_train)
        model.recover()

        # Training accuracy over the 200 training points, class +1 block
        # then class -1 block (which is the storage order of x_train).
        train_correct = 0
        for i in range(len(x_train)):
            if model.g_of(x_train[i]) == y_train[i]:
                train_correct += 1
        train_acc = float(train_correct) / float(len(x_train))

        # Test predictions: class +1 block (50) then class -1 block (50).
        pred = []
        for p in test_c1:
            pred.append(model.g_of(p))
        for p in test_c2:
            pred.append(model.g_of(p))

        correct_c1 = 0
        for k in range(N_TEST_C1):
            if pred[k] == 1:
                correct_c1 += 1
        correct_c2 = 0
        for k in range(N_TEST_C2):
            if pred[N_TEST_C1 + k] == -1:
                correct_c2 += 1

        total_correct = correct_c1 + correct_c2
        test_acc = float(total_correct) / float(N_TEST_C1 + N_TEST_C2)
        test_acc_c1 = float(correct_c1) / float(N_TEST_C1)
        test_acc_c2 = float(correct_c2) / float(N_TEST_C2)

        converged = 0 if model.judge else 1

        lines.append("SVM_VERSION 1")
        lines.append("SWEEPS %d" % SWEEPS)
        lines.append("CONVERGED %d" % converged)
        lines.append("ERROR_LAST " + app.f6(model.error))
        lines.append("MAX_ABS_DELTA " + app.f6(model.max_abs_delta))
        lines.append("BETA " + app.f6(model.beta))
        lines.append("NS_MARGIN %d" % len(model.s_margin))
        lines.append("NS_INSIDE %d" % len(model.s_inside))
        lines.append("W " + " ".join(app.f6(v) for v in model.w))
        lines.append("B " + app.f6(model.b))
        lines.append("OBJECTIVE " + app.f6(model.objective()))
        lines.append("ALPHA_SUM " + app.f6(model.alpha_sum()))
        lines.append("ALPHA_Y_SUM " + app.f12(model.alpha_y_sum()))
        lines.append("ALPHA_CHECKSUM " + app.f6(model.alpha_checksum()))
        lines.append("TRAIN_ACC " + app.f6(train_acc))
        lines.append("TEST_ACC " + app.f6(test_acc))
        lines.append("TEST_ACC_C1 " + app.f6(test_acc_c1))
        lines.append("TEST_ACC_C2 " + app.f6(test_acc_c2))
        lines.append("TEST_CORRECT %d %d %d"
                     % (correct_c1, correct_c2, total_correct))
        lines.append("PRED " + " ".join("%d" % v for v in pred))
        lines.append("ALPHA " + " ".join(app.f6(a) for a in model.alpha))

        import sys
        sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    app.main()
