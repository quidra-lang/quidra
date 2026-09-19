#!/usr/bin/env python3
"""
WL-GMM -- Gaussian Mixture Model via EM.

REFERENCE (oracle) implementation in Python of the FROZEN specification in
    methodology/07_algorithm_workloads.md  section 3  (constants 3.2, data 3.3,
    init 3.4, EM 3.5, ordering 3.6, invariants 3.7, output 3.8)
with the universal conventions of section 1 (binary64 throughout, LCG-PM RNG of
1.2, pinned accumulation order of 1.3, output contract of 1.4).

This file is measurement infrastructure, not a benchmark submission.  It is
written to be a literal, obviously-correct transcription of the specification:
every loop is in the pinned index order, every accumulation is into a single
scalar, nothing is vectorised, reassociated, cached across iterations or
otherwise optimised.  Standard library only (math, sys); no numpy.

Logical modules required by 3.1, spelled identically in all implementations:
    data    -- LCG-PM generator and dataset builder
    linalg  -- Cholesky factorization, forward substitution, log-determinant
    gmm     -- initialization, E-step, M-step, log-likelihood, assignment
    app     -- entry point, canonical component ordering, output formatting
(Python has no sub-file modules here -- 1.9 requires a single source file -- so
each logical module is a class used as a namespace.)
"""

import math
import sys

# ---------------------------------------------------------------------------
# Frozen constants (specification 3.2)
# ---------------------------------------------------------------------------

N = 10000                  # number of data points
D = 3                      # dimension
K = 4                      # number of mixture components
K_TRUE = 4                 # number of generating clusters
SEED = 20260917            # LCG-PM seed
CENTERS = [
    [-3.0, -3.0, -3.0],
    [3.0, -3.0, 3.0],
    [-3.0, 3.0, 3.0],
    [3.0, 3.0, -3.0],
]
SCALES = [1.0, 1.5, 0.8, 1.2]
ITERS = 60                 # fixed number of EM iterations, no early exit
EPS_CONV = 0.000001        # convergence threshold, reported but not controlling
REG = 0.000001             # covariance ridge, added after the Nk division
LOG_2PI = 1.8378770664093453   # ln(2*pi), pinned as an exact decimal literal


# ---------------------------------------------------------------------------
# module data -- LCG-PM generator and dataset builder (specification 1.2, 3.3)
# ---------------------------------------------------------------------------

class data:

    MULT = 48271
    MOD = 2147483647

    class LCG:
        """Lehmer / Park-Miller minimal standard generator.

        state is an integer with 1 <= state <= 2147483646.  Every intermediate
        product fits exactly in binary64 as well as in int64, so the stream is
        identical in every language.
        """

        def __init__(self, seed):
            self.state = seed

        def next_state(self):
            self.state = (data.MULT * self.state) % data.MOD
            return self.state

        def next_uniform(self):
            return float(self.next_state()) / 2147483647.0

        def next_normal(self):
            # Irwin-Hall(12) - 6.  Accumulation is ascending and must not be
            # reassociated (1.3 rule 1).
            t = 0.0
            for _ in range(12):
                t = t + self.next_uniform()
            return t - 6.0

    @staticmethod
    def generate():
        """Dataset of 3.3: one stream, drawn n ascending then d ascending."""
        rng = data.LCG(SEED)
        x = []
        for n in range(N):
            c = n % K_TRUE                      # round-robin cluster
            row = [0.0] * D
            for d in range(D):
                row[d] = CENTERS[c][d] + SCALES[c] * rng.next_normal()
            x.append(row)
        return x


# ---------------------------------------------------------------------------
# module linalg -- Cholesky, forward substitution, log-determinant (3.5)
# ---------------------------------------------------------------------------

class NotPositiveDefinite(Exception):
    """Raised by linalg.cholesky when the guard of 3.5 trips."""


class linalg:

    @staticmethod
    def cholesky(S):
        """Lower-triangular L with L * L^T = S, exactly as pinned in 3.5."""
        L = [[0.0] * D for _ in range(D)]
        for i in range(D):
            for j in range(i + 1):
                s = S[i][j]
                for k in range(j):              # k ascending
                    s = s - L[i][k] * L[j][k]
                if i == j:
                    if s <= 0.0:
                        raise NotPositiveDefinite()
                    L[i][i] = math.sqrt(s)
                else:
                    L[i][j] = s / L[j][j]
        return L

    @staticmethod
    def log_det(L):
        """logdet = 2 * sum over i ascending of log(L[i][i]).

        Deliberately not log(prod(L[i][i]**2)), which overflows/underflows.
        """
        acc = 0.0
        for i in range(D):
            acc = acc + math.log(L[i][i])
        return 2.0 * acc

    @staticmethod
    def mahalanobis(xn, muk, L, z):
        """Solve L z = (x[n] - mu[k]) by forward substitution, return z.z.

        z is a caller-owned scratch buffer of length D; it is fully overwritten.
        """
        q = 0.0
        for i in range(D):
            s = xn[i] - muk[i]
            for j in range(i):                  # j ascending
                s = s - L[i][j] * z[j]
            z[i] = s / L[i][i]
            q = q + z[i] * z[i]
        return q


# ---------------------------------------------------------------------------
# module gmm -- initialization, E-step, M-step, log-likelihood, assignment
# ---------------------------------------------------------------------------

class gmm:

    @staticmethod
    def initialize(x):
        """Deterministic initialization of 3.4.  No RNG is used here at all.

        The reference repository's std::mt19937 + std::normal_distribution
        seeding is removed entirely (specification 1.2).
        """
        # Global per-dimension moments, accumulated over n ascending.
        mean = [0.0] * D
        var = [0.0] * D
        x_sum = [0.0] * D
        x2_sum = [0.0] * D
        for n in range(N):
            xn = x[n]
            for d in range(D):
                x_sum[d] = x_sum[d] + xn[d]
                x2_sum[d] = x2_sum[d] + xn[d] * xn[d]
        for d in range(D):
            mean[d] = x_sum[d] / float(N)
            var[d] = x2_sum[d] / float(N) - mean[d] * mean[d]

        pi = [1.0 / float(K) for _ in range(K)]
        # Forgy-style seeding: the first K data points, one per generating
        # cluster by the round-robin assignment of 3.3.
        mu = [[x[k][d] for d in range(D)] for k in range(K)]
        sigma = [[[var[b] if a == b else 0.0 for b in range(D)]
                  for a in range(D)] for _ in range(K)]
        return pi, mu, sigma

    @staticmethod
    def e_step(x, pi, mu, sigma):
        """E-step of 3.5.  Returns (gamma, total log-likelihood).

        Log-domain with max-subtraction is mandatory.  The per-component
        Cholesky and log-determinant are computed once per E-step, not once
        per point.
        """
        chol = []
        logdet = []
        log_pi = []
        for k in range(K):
            L = linalg.cholesky(sigma[k])
            chol.append(L)
            logdet.append(linalg.log_det(L))
            log_pi.append(math.log(pi[k]))

        gamma = []
        loglik_total = 0.0
        logp = [0.0] * K
        expv = [0.0] * K
        z = [0.0] * D

        for n in range(N):                      # n ascending
            xn = x[n]
            for k in range(K):                  # k ascending
                q = linalg.mahalanobis(xn, mu[k], chol[k], z)
                logp[k] = log_pi[k] - 0.5 * (float(D) * LOG_2PI + logdet[k] + q)

            m = logp[0]                         # scan k ascending, strict >
            for k in range(1, K):
                if logp[k] > m:
                    m = logp[k]

            ssum = 0.0
            for k in range(K):                  # k ascending
                expv[k] = math.exp(logp[k] - m)
                ssum = ssum + expv[k]

            loglik_n = m + math.log(ssum)
            loglik_total = loglik_total + loglik_n

            row = [0.0] * K
            for k in range(K):
                row[k] = expv[k] / ssum
            gamma.append(row)

        return gamma, loglik_total

    @staticmethod
    def m_step(x, gamma, pi, mu, sigma):
        """M-step of 3.5, in place, for k ascending."""
        for k in range(K):
            Nk = 0.0
            for n in range(N):                  # n ascending
                Nk = Nk + gamma[n][k]
            pi[k] = Nk / float(N)

            for d in range(D):
                acc = 0.0
                for n in range(N):              # n ascending
                    acc = acc + gamma[n][k] * x[n][d]
                mu[k][d] = acc / Nk

            # The covariance uses the JUST-UPDATED mu[k].
            muk = mu[k]
            snew = [[0.0] * D for _ in range(D)]
            dev = [0.0] * D
            for n in range(N):                  # n ascending
                xn = x[n]
                g = gamma[n][k]
                for d in range(D):
                    dev[d] = xn[d] - muk[d]
                for a in range(D):
                    for b in range(D):
                        snew[a][b] = snew[a][b] + g * dev[a] * dev[b]

            for a in range(D):
                for b in range(D):
                    snew[a][b] = snew[a][b] / Nk
                snew[a][a] = snew[a][a] + REG   # ridge AFTER the division
            sigma[k] = snew

    @staticmethod
    def gamma_row_dev_max(gamma):
        """max over n of |(sum over k of gamma[n][k]) - 1.0|  (3.7)."""
        worst = 0.0
        for n in range(N):
            s = 0.0
            for k in range(K):                  # k ascending
                s = s + gamma[n][k]
            dev = abs(s - 1.0)
            if dev > worst:
                worst = dev
        return worst

    @staticmethod
    def assign(gamma):
        """Hard assignment of 3.6: argmax over k ascending, strict > to replace
        the incumbent, so the lowest index wins ties.  gamma is already in the
        sorted (canonical) component numbering."""
        assign = [0] * N
        for n in range(N):
            row = gamma[n]
            best = 0
            best_val = row[0]
            for k in range(1, K):
                if row[k] > best_val:
                    best = k
                    best_val = row[k]
            assign[n] = best
        return assign

    @staticmethod
    def min_margin(gamma):
        """min over n of top1(gamma[n]) - top2(gamma[n])  (3.7)."""
        worst = None
        for n in range(N):
            row = gamma[n]
            top1 = -1.0
            top2 = -1.0
            for k in range(K):                  # k ascending, strict >
                v = row[k]
                if v > top1:
                    top2 = top1
                    top1 = v
                elif v > top2:
                    top2 = v
            margin = top1 - top2
            if worst is None or margin < worst:
                worst = margin
        return worst


# ---------------------------------------------------------------------------
# module app -- entry point, canonical ordering, output formatting (1.4, 3.6, 3.8)
# ---------------------------------------------------------------------------

class app:

    @staticmethod
    def fmt(value, places):
        """Fixed-point with exactly `places` decimals, never scientific.

        Negative zero is forbidden in output (1.4), so a value that is exactly
        0.0 -- and likewise any value that rounds to all-zero digits -- is
        printed without a sign.
        """
        s = "%.*f" % (places, value)
        if s.startswith("-") and s.lstrip("-0.") == "":
            s = s[1:]
        return s

    @staticmethod
    def f6(value):
        return app.fmt(value, 6)

    @staticmethod
    def f12(value):
        return app.fmt(value, 12)

    @staticmethod
    def main():
        x = data.generate()
        pi, mu, sigma = gmm.initialize(x)

        # The log-likelihood is computed once before the loop (LOGLIK_INIT) and
        # once after every iteration.  The E-step that produces a log-likelihood
        # value also produces the gamma consistent with those same parameters,
        # and that gamma is what the next iteration's M-step consumes -- exactly
        # the (2.4)/(1.1) structure of the reference repository.  This costs
        # ITERS + 1 = 61 E-step evaluations in total.
        gamma, loglik = gmm.e_step(x, pi, mu, sigma)
        loglik_init = loglik

        prev = loglik
        delta_last = 0.0
        monotone = 1
        for _ in range(ITERS):                  # exactly 60, no early exit
            gmm.m_step(x, gamma, pi, mu, sigma)
            gamma, loglik = gmm.e_step(x, pi, mu, sigma)
            delta = loglik - prev
            if delta < -1e-9:                   # decreased by more than 1e-9
                monotone = 0
            delta_last = delta
            prev = loglik

        converged = 1 if delta_last <= EPS_CONV else 0

        # Canonical component ordering (3.6): sort by mu lexicographically.
        order = sorted(range(K), key=lambda k: (mu[k][0], mu[k][1], mu[k][2]))
        pi_s = [pi[k] for k in order]
        mu_s = [mu[k] for k in order]
        sigma_s = [sigma[k] for k in order]
        gamma_s = [[gamma[n][k] for k in order] for n in range(N)]

        assign = gmm.assign(gamma_s)
        counts = [0] * K
        for n in range(N):
            counts[assign[n]] += 1
        checksum = 0                            # exact integer arithmetic
        for n in range(N):
            checksum += (assign[n] + 1) * ((n % 97) + 1)

        row_dev_max = gmm.gamma_row_dev_max(gamma_s)
        pi_sum = 0.0
        for k in range(K):
            pi_sum = pi_sum + pi_s[k]
        pi_sum_dev = abs(pi_sum - 1.0)
        min_margin = gmm.min_margin(gamma_s)

        out = []
        out.append("GMM_VERSION 1")
        out.append("ITERATIONS %d" % ITERS)
        out.append("CONVERGED %d" % converged)
        out.append("LOGLIK_INIT " + app.f6(loglik_init))
        out.append("LOGLIK " + app.f6(loglik))
        out.append("LOGLIK_PER_POINT " + app.f6(loglik / float(N)))
        out.append("DELTA_LOGLIK_LAST " + app.f12(delta_last))
        out.append("MONOTONE %d" % monotone)
        for k in range(K):
            fields = ["COMP", str(k), "PI", app.f6(pi_s[k]), "MU"]
            for d in range(D):
                fields.append(app.f6(mu_s[k][d]))
            fields.append("SIGMA")
            for a in range(D):                  # row-major 3x3
                for b in range(D):
                    fields.append(app.f6(sigma_s[k][a][b]))
            out.append(" ".join(fields))
        out.append("ASSIGN_COUNTS " + " ".join(str(c) for c in counts))
        out.append("ASSIGN_CHECKSUM %d" % checksum)
        out.append("ASSIGN_FIRST_20 " + " ".join(str(assign[n]) for n in range(20)))
        out.append("GAMMA_ROW_DEV_MAX " + app.f12(row_dev_max))
        out.append("PI_SUM_DEV " + app.f12(pi_sum_dev))
        out.append("ASSIGN_MIN_MARGIN " + app.f12(min_margin))

        sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    try:
        app.main()
    except NotPositiveDefinite:
        sys.stderr.write("ERROR: NOT_POSITIVE_DEFINITE\n")
        sys.exit(3)
