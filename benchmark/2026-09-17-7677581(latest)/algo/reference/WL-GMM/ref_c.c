/*
 * WL-GMM -- Gaussian Mixture Model via EM.
 *
 * REFERENCE implementation in C, written from the FROZEN specification
 *   methodology/07_algorithm_workloads.md  section 3 (and the universal
 *   conventions of section 1), benchmark run 2026-09-17-7677581.
 *
 * This file is measurement infrastructure: it establishes the expected output
 * of the frozen workload.  It is NOT one of the ten measured languages and is
 * never timed.  It is therefore written to be as literal a transcription of the
 * specification as possible: no optimisation, no reassociation, no fusion of
 * loops whose accumulation order the specification pins.
 *
 * Build:  clang -O2 -ffp-contract=off ref_c.c -o REF_c -lm
 *
 * Logical modules (spec 3.1) are expressed as prefix-namespaced sections,
 * C having no namespace construct:  data_, linalg_, gmm_, app_.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ===================================================================== */
/* Frozen constants (spec 3.2)                                           */
/* ===================================================================== */

#define N        10000          /* number of data points                 */
#define D        3              /* dimension                             */
#define K        4              /* number of mixture components          */
#define K_TRUE   4              /* number of generating clusters         */
#define SEED     20260917LL     /* LCG-PM seed                           */
#define ITERS    60             /* fixed number of EM iterations         */

static const double CENTERS[K_TRUE][D] = {
    { -3.0, -3.0, -3.0 },
    {  3.0, -3.0,  3.0 },
    { -3.0,  3.0,  3.0 },
    {  3.0,  3.0, -3.0 }
};
static const double SCALES[K_TRUE] = { 1.0, 1.5, 0.8, 1.2 };

static const double EPS_CONV = 0.000001;
static const double REG      = 0.000001;
static const double LOG_2PI  = 1.8378770664093453;

/* ===================================================================== */
/* module `data` -- the LCG-PM generator and the dataset builder          */
/* (spec 1.2, 3.3)                                                       */
/* ===================================================================== */

#define LCG_MULT 48271LL
#define LCG_MOD  2147483647LL

static long long data_state;

static void data_seed(long long seed)
{
    data_state = seed;
}

static long long data_next_state(void)
{
    data_state = (LCG_MULT * data_state) % LCG_MOD;   /* exact in int64 */
    return data_state;
}

static double data_next_uniform(void)
{
    return (double)data_next_state() / 2147483647.0;
}

/* Irwin-Hall(12) - 6.  Accumulation order is ascending and must not be
 * reassociated (spec 1.2). */
static double data_next_normal(void)
{
    double t = 0.0;
    int i;
    for (i = 0; i < 12; i++) {
        t = t + data_next_uniform();
    }
    return t - 6.0;
}

static double data_x[N][D];

/* spec 3.3: one stream seeded SEED, clusters assigned round-robin. */
static void data_build(void)
{
    long long n;
    int d;

    data_seed(SEED);
    for (n = 0; n < N; n++) {
        long long c = n % (long long)K_TRUE;
        for (d = 0; d < D; d++) {
            data_x[n][d] = CENTERS[c][d] + SCALES[c] * data_next_normal();
        }
    }
}

/* ===================================================================== */
/* module `linalg` -- Cholesky factorization, forward substitution,       */
/*                    log-determinant  (spec 3.5)                        */
/* ===================================================================== */

/* Cholesky of the D x D symmetric positive definite matrix S into the lower
 * triangular L.  Returns 0 on success, non-zero if the positive-definite
 * guard trips (spec 3.8). */
static int linalg_cholesky(const double S[D][D], double L[D][D])
{
    int i, j, k;

    for (i = 0; i < D; i++) {
        for (j = 0; j < D; j++) {
            L[i][j] = 0.0;
        }
    }

    for (i = 0; i < D; i++) {
        for (j = 0; j <= i; j++) {
            double s = S[i][j];
            for (k = 0; k <= j - 1; k++) {          /* k ascending */
                s = s - L[i][k] * L[j][k];
            }
            if (i == j) {
                if (s <= 0.0) {
                    return 1;                        /* NOT_POSITIVE_DEFINITE */
                }
                L[i][i] = sqrt(s);
            } else {
                L[i][j] = s / L[j][j];
            }
        }
    }
    return 0;
}

/* logdet = 2 * sum over i ascending of log(L[i][i])  (spec 3.5) */
static double linalg_logdet_from_cholesky(const double L[D][D])
{
    double acc = 0.0;
    int i;
    for (i = 0; i < D; i++) {
        acc = acc + log(L[i][i]);
    }
    return 2.0 * acc;
}

/* Squared Mahalanobis distance of (x - mu) under S = L L^T, obtained by
 * forward substitution solving L z = (x - mu)  (spec 3.5). */
static double linalg_mahalanobis(const double L[D][D],
                                 const double x[D],
                                 const double mu[D])
{
    double z[D];
    double q = 0.0;
    int i, j;

    for (i = 0; i < D; i++) {
        double s = x[i] - mu[i];
        for (j = 0; j <= i - 1; j++) {               /* j ascending */
            s = s - L[i][j] * z[j];
        }
        z[i] = s / L[i][i];
        q = q + z[i] * z[i];
    }
    return q;
}

/* ===================================================================== */
/* module `gmm` -- initialization, E-step, M-step, log-likelihood,        */
/*                 assignment  (spec 3.4, 3.5, 3.6)                      */
/* ===================================================================== */

static double gmm_pi[K];
static double gmm_mu[K][D];
static double gmm_sigma[K][D][D];
static double gmm_gamma[N][K];
static double gmm_Nk[K];

/* Cholesky factor and log-determinant of each component's covariance,
 * recomputed once per E-step (spec 3.5: "computed once per E-step, not once
 * per point"). */
static double gmm_L[K][D][D];
static double gmm_logdet[K];

static void gmm_die_not_positive_definite(void)
{
    fprintf(stderr, "ERROR: NOT_POSITIVE_DEFINITE\n");
    exit(3);
}

/* spec 3.4 -- fully deterministic initialization; no RNG. */
static void gmm_init(void)
{
    double mean[D];
    double var[D];
    long long n;
    int d, k, a, b;

    for (d = 0; d < D; d++) {
        double sum1 = 0.0;
        double sum2 = 0.0;
        for (n = 0; n < N; n++) {                    /* n ascending */
            sum1 = sum1 + data_x[n][d];
        }
        for (n = 0; n < N; n++) {                    /* n ascending */
            sum2 = sum2 + data_x[n][d] * data_x[n][d];
        }
        mean[d] = sum1 / (double)N;
        var[d]  = sum2 / (double)N - mean[d] * mean[d];
    }

    for (k = 0; k < K; k++) {
        gmm_pi[k] = 1.0 / (double)K;
    }
    for (k = 0; k < K; k++) {
        for (d = 0; d < D; d++) {
            gmm_mu[k][d] = data_x[k][d];             /* Forgy-style seeding */
        }
    }
    for (k = 0; k < K; k++) {
        for (a = 0; a < D; a++) {
            for (b = 0; b < D; b++) {
                gmm_sigma[k][a][b] = (a == b) ? var[b] : 0.0;
            }
        }
    }
}

/* E-step (spec 3.5).  Fills gmm_gamma and returns the total log-likelihood
 * of the current parameters. */
static double gmm_e_step(void)
{
    double L_total = 0.0;
    long long n;
    int k;

    /* Per-component factorization, once per E-step. */
    for (k = 0; k < K; k++) {
        if (linalg_cholesky(gmm_sigma[k], gmm_L[k]) != 0) {
            gmm_die_not_positive_definite();
        }
        gmm_logdet[k] = linalg_logdet_from_cholesky(gmm_L[k]);
    }

    for (n = 0; n < N; n++) {                        /* n ascending */
        double logp[K];
        double m;
        double ssum;
        double loglik_n;

        for (k = 0; k < K; k++) {                    /* k ascending */
            double q = linalg_mahalanobis(gmm_L[k], data_x[n], gmm_mu[k]);
            logp[k] = log(gmm_pi[k])
                      - 0.5 * ((double)D * LOG_2PI + gmm_logdet[k] + q);
        }

        m = logp[0];
        for (k = 1; k < K; k++) {                    /* k ascending, strict > */
            if (logp[k] > m) {
                m = logp[k];
            }
        }

        ssum = 0.0;
        for (k = 0; k < K; k++) {                    /* k ascending */
            ssum = ssum + exp(logp[k] - m);
        }

        loglik_n = m + log(ssum);

        for (k = 0; k < K; k++) {
            gmm_gamma[n][k] = exp(logp[k] - m) / ssum;
        }

        L_total = L_total + loglik_n;                /* n ascending */
    }

    return L_total;
}

/* M-step (spec 3.5). */
static void gmm_m_step(void)
{
    int k, a, b, d;
    long long n;

    for (k = 0; k < K; k++) {                        /* k ascending */
        double Snew[D][D];

        gmm_Nk[k] = 0.0;
        for (n = 0; n < N; n++) {                    /* n ascending */
            gmm_Nk[k] = gmm_Nk[k] + gmm_gamma[n][k];
        }

        gmm_pi[k] = gmm_Nk[k] / (double)N;

        for (d = 0; d < D; d++) {
            double acc = 0.0;
            for (n = 0; n < N; n++) {                /* n ascending */
                acc = acc + gmm_gamma[n][k] * data_x[n][d];
            }
            gmm_mu[k][d] = acc / gmm_Nk[k];
        }

        /* Covariance uses the JUST-UPDATED mu[k]. */
        for (a = 0; a < D; a++) {
            for (b = 0; b < D; b++) {
                Snew[a][b] = 0.0;
            }
        }
        for (n = 0; n < N; n++) {                    /* n ascending */
            double dev[D];
            for (d = 0; d < D; d++) {
                dev[d] = data_x[n][d] - gmm_mu[k][d];
            }
            for (a = 0; a < D; a++) {
                for (b = 0; b < D; b++) {
                    Snew[a][b] = Snew[a][b] + gmm_gamma[n][k] * dev[a] * dev[b];
                }
            }
        }
        for (a = 0; a < D; a++) {
            for (b = 0; b < D; b++) {
                Snew[a][b] = Snew[a][b] / gmm_Nk[k];
            }
            Snew[a][a] = Snew[a][a] + REG;           /* REG after the division */
        }

        for (a = 0; a < D; a++) {
            for (b = 0; b < D; b++) {
                gmm_sigma[k][a][b] = Snew[a][b];
            }
        }
    }
}

/* ===================================================================== */
/* module `app` -- entry point, canonical component ordering, formatting  */
/* ===================================================================== */

/* F6 / F12 formatting with the negative-zero normalization of spec 1.4:
 * if the rounded decimal is all zeros, the sign is dropped. */
static void app_fmt(char *buf, size_t buflen, double v, int digits)
{
    const char *p;
    int all_zero;

    snprintf(buf, buflen, "%.*f", digits, v);

    if (buf[0] == '-') {
        all_zero = 1;
        for (p = buf + 1; *p; p++) {
            if (*p != '0' && *p != '.') {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            memmove(buf, buf + 1, strlen(buf));      /* drop the '-' */
        }
    }
}

static void app_print_f6(double v)
{
    char buf[64];
    app_fmt(buf, sizeof(buf), v, 6);
    fputs(buf, stdout);
}

static void app_print_f12(double v)
{
    char buf[64];
    app_fmt(buf, sizeof(buf), v, 12);
    fputs(buf, stdout);
}

/* key(k) = (mu[k][0], mu[k][1], mu[k][2]) ascending lexicographic. */
static int app_mu_less(const double a[D], const double b[D])
{
    int d;
    for (d = 0; d < D; d++) {
        if (a[d] < b[d]) return 1;
        if (a[d] > b[d]) return 0;
    }
    return 0;
}

/* Sort the components into the canonical order and renumber 0..K-1,
 * permuting pi, mu, sigma and the columns of gamma (spec 3.6). */
static void app_canonical_sort(void)
{
    int order[K];
    int i, j, k, a, b;
    long long n;

    double new_pi[K];
    double new_mu[K][D];
    double new_sigma[K][D][D];

    for (k = 0; k < K; k++) {
        order[k] = k;
    }
    /* insertion sort; K = 4 */
    for (i = 1; i < K; i++) {
        int key = order[i];
        j = i - 1;
        while (j >= 0 && app_mu_less(gmm_mu[key], gmm_mu[order[j]])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    for (k = 0; k < K; k++) {
        int src = order[k];
        new_pi[k] = gmm_pi[src];
        for (a = 0; a < D; a++) {
            new_mu[k][a] = gmm_mu[src][a];
            for (b = 0; b < D; b++) {
                new_sigma[k][a][b] = gmm_sigma[src][a][b];
            }
        }
    }
    for (k = 0; k < K; k++) {
        gmm_pi[k] = new_pi[k];
        for (a = 0; a < D; a++) {
            gmm_mu[k][a] = new_mu[k][a];
            for (b = 0; b < D; b++) {
                gmm_sigma[k][a][b] = new_sigma[k][a][b];
            }
        }
    }
    for (n = 0; n < N; n++) {
        double row[K];
        for (k = 0; k < K; k++) {
            row[k] = gmm_gamma[n][order[k]];
        }
        for (k = 0; k < K; k++) {
            gmm_gamma[n][k] = row[k];
        }
    }
}

int main(void)
{
    double loglik_init;
    double loglik_prev;
    double loglik;
    double delta_last;
    int monotone = 1;
    int converged;
    int it, k, a, b;
    long long n;

    int assign[N];
    long long assign_counts[K];
    long long assign_checksum;
    double gamma_row_dev_max;
    double pi_sum;
    double pi_sum_dev;
    double assign_min_margin;

    data_build();
    gmm_init();

    /* Log-likelihood once before the loop, then once after every iteration.
     * Each E-step yields gamma and the log-likelihood of the current
     * parameters; each M-step then consumes that gamma. */
    loglik = gmm_e_step();
    loglik_init = loglik;
    loglik_prev = loglik;
    delta_last = 0.0;

    for (it = 0; it < ITERS; it++) {
        gmm_m_step();
        loglik = gmm_e_step();
        delta_last = loglik - loglik_prev;
        if (delta_last < -0.000000001) {
            monotone = 0;
        }
        loglik_prev = loglik;
    }

    converged = (delta_last <= EPS_CONV) ? 1 : 0;

    app_canonical_sort();

    /* Hard assignment: argmax over k ascending, strict > to replace the
     * incumbent, in the sorted numbering. */
    for (k = 0; k < K; k++) {
        assign_counts[k] = 0;
    }
    assign_checksum = 0;
    assign_min_margin = 0.0;
    gamma_row_dev_max = 0.0;

    for (n = 0; n < N; n++) {
        int best = 0;
        double top1, top2;
        double rowsum = 0.0;
        double dev;

        for (k = 1; k < K; k++) {
            if (gmm_gamma[n][k] > gmm_gamma[n][best]) {
                best = k;
            }
        }
        assign[n] = best;
        assign_counts[best]++;
        assign_checksum += (long long)(assign[n] + 1) * ((n % 97) + 1);

        top1 = gmm_gamma[n][best];
        top2 = -1.0;
        for (k = 0; k < K; k++) {
            if (k != best && gmm_gamma[n][k] > top2) {
                top2 = gmm_gamma[n][k];
            }
        }
        if (n == 0 || (top1 - top2) < assign_min_margin) {
            assign_min_margin = top1 - top2;
        }

        for (k = 0; k < K; k++) {
            rowsum = rowsum + gmm_gamma[n][k];
        }
        dev = fabs(rowsum - 1.0);
        if (dev > gamma_row_dev_max) {
            gamma_row_dev_max = dev;
        }
    }

    pi_sum = 0.0;
    for (k = 0; k < K; k++) {
        pi_sum = pi_sum + gmm_pi[k];
    }
    pi_sum_dev = fabs(pi_sum - 1.0);

    /* ---------------- output schema (spec 3.8), 18 lines ---------------- */

    printf("GMM_VERSION 1\n");
    printf("ITERATIONS %d\n", ITERS);
    printf("CONVERGED %d\n", converged);

    fputs("LOGLIK_INIT ", stdout);       app_print_f6(loglik_init);            fputs("\n", stdout);
    fputs("LOGLIK ", stdout);            app_print_f6(loglik);                 fputs("\n", stdout);
    fputs("LOGLIK_PER_POINT ", stdout);  app_print_f6(loglik / (double)N);     fputs("\n", stdout);
    fputs("DELTA_LOGLIK_LAST ", stdout); app_print_f12(delta_last);            fputs("\n", stdout);

    printf("MONOTONE %d\n", monotone);

    for (k = 0; k < K; k++) {
        printf("COMP %d PI ", k);
        app_print_f6(gmm_pi[k]);
        fputs(" MU", stdout);
        for (a = 0; a < D; a++) {
            fputs(" ", stdout);
            app_print_f6(gmm_mu[k][a]);
        }
        fputs(" SIGMA", stdout);
        for (a = 0; a < D; a++) {
            for (b = 0; b < D; b++) {
                fputs(" ", stdout);
                app_print_f6(gmm_sigma[k][a][b]);
            }
        }
        fputs("\n", stdout);
    }

    fputs("ASSIGN_COUNTS", stdout);
    for (k = 0; k < K; k++) {
        printf(" %lld", assign_counts[k]);
    }
    fputs("\n", stdout);

    printf("ASSIGN_CHECKSUM %lld\n", assign_checksum);

    fputs("ASSIGN_FIRST_20", stdout);
    for (n = 0; n < 20; n++) {
        printf(" %d", assign[n]);
    }
    fputs("\n", stdout);

    fputs("GAMMA_ROW_DEV_MAX ", stdout); app_print_f12(gamma_row_dev_max);  fputs("\n", stdout);
    fputs("PI_SUM_DEV ", stdout);        app_print_f12(pi_sum_dev);         fputs("\n", stdout);
    fputs("ASSIGN_MIN_MARGIN ", stdout); app_print_f12(assign_min_margin);  fputs("\n", stdout);

    return 0;
}
