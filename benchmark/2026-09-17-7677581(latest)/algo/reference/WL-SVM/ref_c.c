/*
 * WL-SVM reference implementation (C).
 *
 * Reference oracle for benchmark run 2026-09-17-7677581.  This file is NOT one
 * of the ten measured languages: it exists only to establish the expected
 * output of the frozen workload.  It is a literal transcription of
 *   methodology/07_algorithm_workloads.md section 2 (Workload WL-SVM),
 * with the universal conventions of section 1 (LCG-PM RNG, binary64 only,
 * normative accumulation order, output contract).
 *
 * Build:  clang -O2 -ffp-contract=off ref_c.c -o REF_c -lm
 *
 * Nothing here is optimised.  Accumulation order is exactly as pinned by the
 * specification; no reassociation, no vectorisation, no parallelism.
 *
 * Logical modules required by 2.1 are marked below:
 *   data - LCG-PM generator and dataset builder
 *   svm  - training, support-vector extraction, w/b recovery, f, g, evaluation
 *   app  - entry point and output formatting
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* ===================================================================== */
/* Frozen constants (spec 2.2)                                           */
/* ===================================================================== */

#define D            4
#define N_TRAIN_C1   100
#define N_TRAIN_C2   100
#define N_TEST_C1    50
#define N_TEST_C2    50
#define N_TRAIN      (N_TRAIN_C1 + N_TRAIN_C2)   /* N = 200 */
#define N_TEST       (N_TEST_C1 + N_TEST_C2)     /* 100 */

static const double MU1[D] = { 1.0,  1.0,  0.5, -0.5};
static const double MU2[D] = {-1.0, -1.0, -0.5,  0.5};

static const double SIGMA  = 0.8;
static const int64_t SEED  = 1234567;
static const double C_REG  = 10.0;      /* `C`      */
static const double LR     = 0.0001;    /* `LR`     */
static const double LIMIT  = 0.0001;    /* `LIMIT`  */
static const int64_t SWEEPS = 1000;     /* `SWEEPS` */
static const double EPS_SV = 0.0000001; /* `EPS_SV` */

/* ===================================================================== */
/* module `data` - LCG-PM generator (spec 1.2) and dataset builder (2.3)  */
/* ===================================================================== */

#define DATA_LCG_MULT  ((int64_t)48271)
#define DATA_LCG_MOD   ((int64_t)2147483647)

static int64_t data_state;

static void data_seed(int64_t seed)
{
    data_state = seed;
}

/* state = (MULT * state) mod MOD, exact 64-bit integer arithmetic.
   Largest intermediate is 48271 * 2147483646 ~= 1.0369e14 < 2^63. */
static int64_t data_next_state(void)
{
    data_state = (DATA_LCG_MULT * data_state) % DATA_LCG_MOD;
    return data_state;
}

static double data_next_uniform(void)
{
    return (double)data_next_state() / 2147483647.0;
}

/* Irwin-Hall(12) - 6.  Ascending accumulation, must not be reassociated. */
static double data_next_normal(void)
{
    double t = 0.0;
    int k;
    for (k = 0; k < 12; k++) {
        t = t + data_next_uniform();
    }
    return t - 6.0;
}

/* Fill `n` points of dimension D with mean `mu`, drawing points ascending and
   within a point dimensions ascending. */
static void data_fill_block(double (*dst)[D], int n, const double *mu)
{
    int i, d;
    for (i = 0; i < n; i++) {
        for (d = 0; d < D; d++) {
            dst[i][d] = mu[d] + SIGMA * data_next_normal();
        }
    }
}

/* ===================================================================== */
/* module `svm`                                                          */
/* ===================================================================== */

/* Training data, concatenated class +1 first then class -1 (spec 2.3). */
static double svm_x[N_TRAIN][D];
static double svm_y[N_TRAIN];          /* +1.0 / -1.0, held as double */

/* Test data, kept as two blocks. */
static double svm_test_c1[N_TEST_C1][D];
static double svm_test_c2[N_TEST_C2][D];

/* Gram matrix, precomputed once (mandatory, spec 2.4). */
static double svm_G[N_TRAIN][N_TRAIN];

/* Learned quantities. */
static double svm_alpha[N_TRAIN];
static double svm_beta;
static double svm_w[D];
static double svm_b;

/* Final-sweep diagnostics. */
static int    svm_judge_last;
static double svm_error_last;
static double svm_max_abs_delta_last;

/* Support-vector index sets, in ascending index order. */
static int svm_s_margin[N_TRAIN];
static int svm_ns_margin;
static int svm_s_inside[N_TRAIN];
static int svm_ns_inside;

static void svm_build_gram(void)
{
    int i, j, d;
    for (i = 0; i < N_TRAIN; i++) {
        for (j = 0; j < N_TRAIN; j++) {
            double s = 0.0;
            for (d = 0; d < D; d++) {
                s = s + svm_x[i][d] * svm_x[j][d];
            }
            svm_G[i][j] = s;
        }
    }
}

/* Spec 2.4: exactly SWEEPS full sweeps, no early exit.  alpha is updated in
   place in ascending i (Gauss-Seidel).  beta is updated once per sweep. */
static void svm_train(void)
{
    int64_t sweep;
    int i, j;

    for (i = 0; i < N_TRAIN; i++) {
        svm_alpha[i] = 0.0;
    }
    svm_beta = 1.0;

    for (sweep = 0; sweep < SWEEPS; sweep++) {

        int    judge = 0;
        double error = 0.0;
        double max_abs_delta = 0.0;
        double s;

        /* (3.1) update alpha */
        for (i = 0; i < N_TRAIN; i++) {

            double item1 = 0.0;
            double item2 = 0.0;
            double delta;
            double ad;

            for (j = 0; j < N_TRAIN; j++) {
                item1 = item1 + svm_alpha[j] * svm_y[i] * svm_y[j] * svm_G[i][j];
            }

            for (j = 0; j < N_TRAIN; j++) {
                item2 = item2 + svm_alpha[j] * svm_y[i] * svm_y[j];
            }

            delta = 1.0 - item1 - svm_beta * item2;

            ad = fabs(delta);
            if (ad > max_abs_delta) {
                max_abs_delta = ad;
            }

            svm_alpha[i] = svm_alpha[i] + LR * delta;

            /* The if / else-if / else-if chain is exactly the reference's:
               a clamped multiplier sets neither `judge` nor `error`. */
            if (svm_alpha[i] < 0.0) {
                svm_alpha[i] = 0.0;
            } else if (svm_alpha[i] > C_REG) {
                svm_alpha[i] = C_REG;
            } else if (fabs(delta) > LIMIT) {
                judge = 1;
                error = error + (fabs(delta) - LIMIT);
            }
        }

        /* (3.2) update beta, once per sweep */
        s = 0.0;
        for (i = 0; i < N_TRAIN; i++) {
            s = s + svm_alpha[i] * svm_y[i];
        }
        svm_beta = svm_beta + s * s / 2.0;

        svm_judge_last = judge;
        svm_error_last = error;
        svm_max_abs_delta_last = max_abs_delta;
    }
}

/* Spec 2.5: support-vector extraction and w / b recovery. */
static void svm_recover(void)
{
    int i, d, k;

    svm_ns_margin = 0;
    svm_ns_inside = 0;
    for (i = 0; i < N_TRAIN; i++) {
        if ((EPS_SV < svm_alpha[i]) && (svm_alpha[i] < C_REG - EPS_SV)) {
            svm_s_margin[svm_ns_margin++] = i;
        } else if (svm_alpha[i] >= C_REG - EPS_SV) {
            svm_s_inside[svm_ns_inside++] = i;
        }
    }

    for (d = 0; d < D; d++) {
        svm_w[d] = 0.0;
    }
    for (d = 0; d < D; d++) {
        for (k = 0; k < svm_ns_margin; k++) {
            i = svm_s_margin[k];
            svm_w[d] = svm_w[d] + svm_alpha[i] * svm_y[i] * svm_x[i][d];
        }
        for (k = 0; k < svm_ns_inside; k++) {
            i = svm_s_inside[k];
            svm_w[d] = svm_w[d] + svm_alpha[i] * svm_y[i] * svm_x[i][d];
        }
    }

    if (svm_ns_margin == 0) {
        /* Spec 2.7: |S_margin| == 0 is a FAIL. */
        fprintf(stderr, "Error : no support vectors on margin.\n");
        exit(1);
    }

    svm_b = 0.0;
    for (k = 0; k < svm_ns_margin; k++) {
        double dp = 0.0;
        i = svm_s_margin[k];
        for (d = 0; d < D; d++) {
            dp = dp + svm_w[d] * svm_x[i][d];
        }
        svm_b = svm_b + (svm_y[i] - dp);
    }
    svm_b = svm_b / (double)svm_ns_margin;
}

static double svm_f(const double *p)
{
    double s = 0.0;
    int d;
    for (d = 0; d < D; d++) {
        s = s + svm_w[d] * p[d];
    }
    return s + svm_b;
}

static int svm_g(const double *p)
{
    return (svm_f(p) >= 0.0) ? 1 : -1;
}

/* Spec 2.5: dual objective. */
static double svm_objective(void)
{
    double sum_alpha = 0.0;
    double quad = 0.0;
    int i, j;

    for (i = 0; i < N_TRAIN; i++) {
        sum_alpha = sum_alpha + svm_alpha[i];
    }
    for (i = 0; i < N_TRAIN; i++) {
        double inner = 0.0;
        for (j = 0; j < N_TRAIN; j++) {
            inner = inner + svm_alpha[i] * svm_alpha[j] * svm_y[i] * svm_y[j] * svm_G[i][j];
        }
        quad = quad + inner;
    }
    return sum_alpha - 0.5 * quad;
}

/* ===================================================================== */
/* module `app` - output formatting (spec 1.4) and entry point            */
/* ===================================================================== */

/* Spec 1.4: negative zero is forbidden in output. */
static double app_nz(double v)
{
    return (v == 0.0) ? 0.0 : v;
}

static void app_print_f6(double v)
{
    printf("%.6f", app_nz(v));
}

static void app_print_f12(double v)
{
    printf("%.12f", app_nz(v));
}

int main(void)
{
    int i, d, k;
    double alpha_sum, alpha_y_sum, alpha_checksum, objective;
    int train_correct, correct_c1, correct_c2;
    int pred[N_TEST];

    /* ---- data generation (spec 2.3), one stream, four blocks in order ---- */
    data_seed(SEED);
    data_fill_block(svm_x, N_TRAIN_C1, MU1);                 /* train class +1 */
    data_fill_block(svm_x + N_TRAIN_C1, N_TRAIN_C2, MU2);    /* train class -1 */
    data_fill_block(svm_test_c1, N_TEST_C1, MU1);            /* test  class +1 */
    data_fill_block(svm_test_c2, N_TEST_C2, MU2);            /* test  class -1 */

    for (i = 0; i < N_TRAIN; i++) {
        svm_y[i] = (i < N_TRAIN_C1) ? 1.0 : -1.0;
    }

    /* ---- training ---- */
    svm_build_gram();
    svm_train();
    svm_recover();

    /* ---- aggregates ---- */
    alpha_sum = 0.0;
    for (i = 0; i < N_TRAIN; i++) {
        alpha_sum = alpha_sum + svm_alpha[i];
    }

    alpha_y_sum = 0.0;
    for (i = 0; i < N_TRAIN; i++) {
        alpha_y_sum = alpha_y_sum + svm_alpha[i] * svm_y[i];
    }

    alpha_checksum = 0.0;
    for (i = 0; i < N_TRAIN; i++) {
        alpha_checksum = alpha_checksum + svm_alpha[i] * (double)((i % 97) + 1);
    }

    objective = svm_objective();

    /* ---- accuracies ---- */
    train_correct = 0;
    for (i = 0; i < N_TRAIN; i++) {
        int label = (i < N_TRAIN_C1) ? 1 : -1;
        if (svm_g(svm_x[i]) == label) {
            train_correct++;
        }
    }

    correct_c1 = 0;
    for (i = 0; i < N_TEST_C1; i++) {
        int gx = svm_g(svm_test_c1[i]);
        pred[i] = gx;
        if (gx == 1) {
            correct_c1++;
        }
    }
    correct_c2 = 0;
    for (i = 0; i < N_TEST_C2; i++) {
        int gx = svm_g(svm_test_c2[i]);
        pred[N_TEST_C1 + i] = gx;
        if (gx == -1) {
            correct_c2++;
        }
    }

    /* ---- output schema (spec 2.7), 21 lines exactly ---- */
    printf("SVM_VERSION 1\n");
    printf("SWEEPS %lld\n", (long long)SWEEPS);
    printf("CONVERGED %d\n", svm_judge_last ? 0 : 1);
    printf("ERROR_LAST ");        app_print_f6(svm_error_last);         printf("\n");
    printf("MAX_ABS_DELTA ");     app_print_f6(svm_max_abs_delta_last); printf("\n");
    printf("BETA ");              app_print_f6(svm_beta);               printf("\n");
    printf("NS_MARGIN %d\n", svm_ns_margin);
    printf("NS_INSIDE %d\n", svm_ns_inside);

    printf("W");
    for (d = 0; d < D; d++) {
        printf(" ");
        app_print_f6(svm_w[d]);
    }
    printf("\n");

    printf("B ");              app_print_f6(svm_b);          printf("\n");
    printf("OBJECTIVE ");      app_print_f6(objective);      printf("\n");
    printf("ALPHA_SUM ");      app_print_f6(alpha_sum);      printf("\n");
    printf("ALPHA_Y_SUM ");    app_print_f12(alpha_y_sum);   printf("\n");
    printf("ALPHA_CHECKSUM "); app_print_f6(alpha_checksum); printf("\n");

    printf("TRAIN_ACC ");   app_print_f6((double)train_correct / (double)N_TRAIN);              printf("\n");
    printf("TEST_ACC ");    app_print_f6((double)(correct_c1 + correct_c2) / (double)N_TEST);   printf("\n");
    printf("TEST_ACC_C1 "); app_print_f6((double)correct_c1 / (double)N_TEST_C1);               printf("\n");
    printf("TEST_ACC_C2 "); app_print_f6((double)correct_c2 / (double)N_TEST_C2);               printf("\n");

    printf("TEST_CORRECT %d %d %d\n", correct_c1, correct_c2, correct_c1 + correct_c2);

    printf("PRED");
    for (k = 0; k < N_TEST; k++) {
        printf(" %d", pred[k]);
    }
    printf("\n");

    printf("ALPHA");
    for (i = 0; i < N_TRAIN; i++) {
        printf(" ");
        app_print_f6(svm_alpha[i]);
    }
    printf("\n");

    return 0;
}
