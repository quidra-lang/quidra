/* Reference implementation of the 11 frozen micro-benchmark workloads.
   Used ONLY to derive the expected output values recorded in the frozen methodology. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef long long i64;
static const i64 P = 2147483647LL;
static const i64 MA = 48271LL;
static i64 st;
static void rseed(i64 s) { st = s; }
static i64 nexti(void) { st = (MA * st) % P; return st; }
static double nextu(void) { return (double)nexti() / 2147483647.0; }

static void pf(const char *n, double v) { printf(" %s=%.16e", n, v); }

/* ---------- MB01 ---------- */
static i64 fibr(int n) { if (n < 2) return n; return fibr(n - 1) + fibr(n - 2); }
static void mb01(void) {
    i64 t = 0;
    for (int n = 30; n <= 37; n++) t += fibr(n);
    printf("MB01 %lld\n", t);
}

/* ---------- MB02 ---------- */
static void mb02(void) {
    const i64 M = 1000003, N = 20000;
    i64 total = 0;
    for (i64 k = 1; k <= N; k++) {
        i64 f = 1;
        for (i64 j = 2; j <= k; j++) f = (f * j) % M;
        total = (total + f) % M;
    }
    printf("MB02 %lld\n", total);
}

/* ---------- MB03 ---------- */
static void mb03(void) {
    const i64 N = 120000000LL;
    rseed(20263917LL);
    i64 x = st, s_add = 0, s_xor = 0, s_mul = 1, s_div = 0;
    for (i64 i = 0; i < N; i++) {
        x = (48271LL * x) % 2147483647LL;
        s_add = (s_add + x) % 2147483647LL;
        s_xor = s_xor ^ x;
        s_mul = (s_mul * 33 + (x % 97)) % 1000003LL;
        s_div = s_div + (x / 1000);
    }
    printf("MB03 %lld %lld %lld %lld\n", s_add, s_xor, s_mul, s_div);
}

/* ---------- MB04 ---------- */
static void mb04(void) {
    const int M = 4000; const int R = 75000;
    static double A[4000], B[4000];
    rseed(20264917LL);
    for (int i = 0; i < M; i++) A[i] = 0.5 + nextu();
    for (int i = 0; i < M; i++) B[i] = 0.5 + nextu();
    double s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    for (int r = 0; r < R; r++) {
        A[r % M] = A[r % M] + 1.0e-9;
        for (int i = 0; i < M; i++) {
            double a = A[i], b = B[i];
            s1 = s1 + a * b;
            s2 = s2 + a / (b + 2.0);
            s3 = s3 + sqrt(a * a + b * b);
            s4 = s4 + (a - b) * (a - b);
        }
    }
    printf("MB04"); pf("s1", s1); pf("s2", s2); pf("s3", s3); pf("s4", s4); printf("\n");
}

/* ---------- MB05 ---------- */
static void mb05(void) {
    const int N = 2000000; const int R = 400;
    double *X = malloc(sizeof(double) * N), *Y = malloc(sizeof(double) * N);
    rseed(20265917LL);
    for (int i = 0; i < N; i++) X[i] = 0.5 + nextu();
    for (int i = 0; i < N; i++) Y[i] = 0.5 + nextu();
    double total = 0.0;
    for (int r = 0; r < R; r++) {
        X[r] = X[r] + 1.0e-9;
        double d = 0.0;
        for (int i = 0; i < N; i++) d = d + X[i] * Y[i];
        total = total + d;
    }
    printf("MB05"); pf("total", total); printf("\n");
    free(X); free(Y);
}

/* ---------- MB06 ---------- */
static void mb06(void) {
    const int n = 512; const int R = 3;
    double *A = malloc(sizeof(double) * n * n), *B = malloc(sizeof(double) * n * n), *C = malloc(sizeof(double) * n * n);
    rseed(20266917LL);
    for (int i = 0; i < n * n; i++) A[i] = nextu();
    for (int i = 0; i < n * n; i++) B[i] = nextu();
    for (int r = 0; r < R; r++) {
        A[r] = A[r] + 1.0e-9;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                double s = 0.0;
                for (int k = 0; k < n; k++) s = s + A[i * n + k] * B[k * n + j];
                C[i * n + j] = s;
            }
    }
    double sum = 0.0;
    for (int i = 0; i < n * n; i++) sum = sum + C[i];
    printf("MB06"); pf("sumC", sum); pf("c_first", C[0]); pf("c_last", C[n * n - 1]); printf("\n");
    free(A); free(B); free(C);
}

/* ---------- MB07 ---------- */
static void msort(i64 *a, i64 *buf, int n) {
    i64 *src = a, *dst = buf;
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width; if (mid > n) mid = n;
            int hi = lo + 2 * width; if (hi > n) hi = n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) { if (src[i] <= src[j]) dst[k++] = src[i++]; else dst[k++] = src[j++]; }
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        i64 *t = src; src = dst; dst = t;
    }
    if (src != a) for (int i = 0; i < n; i++) a[i] = src[i];
}
static void mb07(void) {
    const int N = 2000000; const int R = 4;
    i64 *src = malloc(sizeof(i64) * N), *a = malloc(sizeof(i64) * N), *buf = malloc(sizeof(i64) * N);
    rseed(20267917LL);
    for (int i = 0; i < N; i++) src[i] = nexti();
    i64 total = 0, sum = 0, inv = 0;
    for (int r = 0; r < R; r++) {
        src[r] = src[r] + 1;
        for (int i = 0; i < N; i++) a[i] = src[i];
        msort(a, buf, N);
        i64 chk = 0;
        for (int i = 0; i < N; i++) chk = (chk * 31 + (a[i] % 1000003LL)) % 1000003LL;
        total = (total * 7 + chk) % 1000003LL;
        sum = 0;
        for (int i = 0; i < N; i++) sum += a[i];
        for (int i = 1; i < N; i++) if (a[i - 1] > a[i]) inv++;
    }
    printf("MB07 %lld %lld %lld\n", total, sum, inv);
    free(src); free(a); free(buf);
}

/* ---------- MB08 ---------- */
static i64 rhash(const unsigned char *s, i64 len) {
    i64 h = 0;
    for (i64 i = 0; i < len; i++) h = (h * 131 + (i64)s[i]) % 1000000007LL;
    return h;
}
static void mb08(void) {
    const int NW = 200000; const int R = 20;
    unsigned char *text = malloc(NW * 20 + 16);
    i64 len = 0;
    rseed(20268917LL);
    for (int w = 0; w < NW; w++) {
        if (w > 0) text[len++] = ' ';
        int L = 4 + (int)(nexti() % 13);
        for (int c = 0; c < L; c++) text[len++] = (unsigned char)('a' + (int)(nexti() % 26));
    }
    unsigned char *U = malloc(len + 1), *Rv = malloc(len + 1);
    i64 acc = 0, cnt_ab = 0, cnt_w = 0;
    const i64 Q = 1000000007LL;
    for (int r = 0; r < R; r++) {
        i64 p = 7LL * r + 11;
        if (text[p] == ' ') text[p] = 'x';
        else text[p] = (unsigned char)('a' + ((text[p] - 'a' + 1) % 26));
        i64 h1 = rhash(text, len);
        for (i64 i = 0; i < len; i++) { unsigned char c = text[i]; U[i] = (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c; }
        i64 h2 = rhash(U, len);
        for (i64 i = 0; i < len; i++) Rv[i] = text[len - 1 - i];
        i64 h3 = rhash(Rv, len);
        cnt_ab = 0;
        for (i64 i = 0; i + 1 < len; i++) if (text[i] == 'a' && text[i + 1] == 'b') cnt_ab++;
        cnt_w = 1;
        for (i64 i = 0; i < len; i++) if (text[i] == ' ') cnt_w++;
        acc = (acc * 31 + h1) % Q;
        acc = (acc * 31 + h2) % Q;
        acc = (acc * 31 + h3) % Q;
        acc = (acc * 31 + cnt_ab) % Q;
        acc = (acc * 31 + cnt_w) % Q;
    }
    printf("MB08 %lld %lld %lld %lld\n", acc, len, cnt_ab, cnt_w);
    free(text); free(U); free(Rv);
}

/* ---------- MB09 ---------- */
static void mb09(void) {
    const int N = 2000000; const int R = 30;
    double *X = malloc(sizeof(double) * N), *Y = malloc(sizeof(double) * N);
    rseed(20269917LL);
    for (int i = 0; i < N; i++) X[i] = nextu() * 100.0;
    for (int i = 0; i < N; i++) Y[i] = nextu() * 100.0;
    double mean = 0, var = 0, sd = 0, mn = 0, mx = 0, mad = 0, pear = 0;
    i64 hchk = 0;
    for (int r = 0; r < R; r++) {
        X[r] = X[r] + 1.0e-9;
        double sum = 0.0; mn = X[0]; mx = X[0];
        for (int i = 0; i < N; i++) { double v = X[i]; sum = sum + v; if (v < mn) mn = v; if (v > mx) mx = v; }
        mean = sum / (double)N;
        double sq = 0.0, ad = 0.0;
        for (int i = 0; i < N; i++) { double d = X[i] - mean; sq = sq + d * d; ad = ad + (d < 0 ? -d : d); }
        var = sq / (double)N; sd = sqrt(var); mad = ad / (double)N;
        double sy = 0.0;
        for (int i = 0; i < N; i++) sy = sy + Y[i];
        double meany = sy / (double)N;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (int i = 0; i < N; i++) { double dx = X[i] - mean, dy = Y[i] - meany; sxy = sxy + dx * dy; sxx = sxx + dx * dx; syy = syy + dy * dy; }
        pear = sxy / sqrt(sxx * syy);
        i64 hist[64]; for (int b = 0; b < 64; b++) hist[b] = 0;
        for (int i = 0; i < N; i++) {
            int b = (int)floor(X[i] * 0.64);
            if (b < 0) b = 0; if (b > 63) b = 63;
            hist[b]++;
        }
        hchk = 0;
        for (int b = 0; b < 64; b++) hchk += (i64)(b + 1) * hist[b];
    }
    printf("MB09"); pf("mean", mean); pf("var", var); pf("sd", sd); pf("min", mn); pf("max", mx);
    pf("mad", mad); pf("pearson", pear); printf(" hist_chk=%lld\n", hchk);
    free(X); free(Y);
}

/* ---------- MB10 ---------- */
static void mb10(void) {
    const int N = 1000000; const int R = 3;
    rseed(20270917LL);
    i64 sum_v = 0, chk = 0, bytes = 0, lines = 0;
    char name[64], line[128];
    for (int r = 0; r < R; r++) {
        snprintf(name, sizeof name, "mb10_round_%d.txt", r);
        FILE *f = fopen(name, "wb");
        for (int i = 0; i < N; i++) {
            i64 v = nexti();
            int n = snprintf(line, sizeof line, "%d %lld\n", i, v);
            fwrite(line, 1, (size_t)n, f);
            bytes += n;
        }
        fclose(f);
        f = fopen(name, "rb");
        char buf[256];
        i64 idx = 0;
        while (fgets(buf, sizeof buf, f)) {
            char *e;
            i64 a = strtoll(buf, &e, 10);
            i64 v = strtoll(e, &e, 10);
            if (a != idx) { fprintf(stderr, "MB10 index mismatch\n"); exit(1); }
            idx++; lines++;
            sum_v = (sum_v + v) % 1000000007LL;
            chk = (chk * 31 + (v % 1000003LL)) % 1000003LL;
        }
        fclose(f);
    }
    printf("MB10 %lld %lld %lld %lld\n", sum_v, chk, bytes, lines);
}

/* ---------- MB11 ---------- */
static void mb11(void) {
    const int N = 1000000; const int R = 3;
    const int KM = 500009, SM = 100003;
    const i64 Q = 1000000007LL;
    i64 *keys = malloc(sizeof(i64) * N);
    rseed(20271917LL);
    for (int i = 0; i < N; i++) keys[i] = nexti();
    i64 *cnt = malloc(sizeof(i64) * KM);
    char *present = malloc(KM);
    char *sset = malloc(SM);
    i64 acc = 0, size1 = 0, found = 0, vsum = 0, mchk = 0, size2 = 0, size3 = 0, lsum = 0;
    for (int r = 0; r < R; r++) {
        keys[r] = keys[r] + 1000000;
        memset(cnt, 0, sizeof(i64) * KM); memset(present, 0, KM);
        size1 = 0;
        for (int i = 0; i < N; i++) {
            i64 k = keys[i] % KM;
            if (!present[k]) { present[k] = 1; size1++; cnt[k] = 1; } else cnt[k] += 1;
        }
        found = 0; vsum = 0;
        for (int i = 0; i < N; i++) {
            i64 k = (keys[i] + 7) % KM;
            if (present[k]) { found++; vsum += cnt[k]; }
        }
        mchk = 0;
        for (i64 k = 0; k < KM; k++) if (present[k]) mchk = (mchk + (k % 1000003LL) * cnt[k]) % 1000003LL;
        for (int i = 0; i < N; i += 2) {
            i64 k = keys[i] % KM;
            if (present[k]) { present[k] = 0; cnt[k] = 0; }
        }
        size2 = 0;
        for (i64 k = 0; k < KM; k++) if (present[k]) size2++;
        memset(sset, 0, SM);
        size3 = 0;
        for (int i = 0; i < N; i++) { i64 k = keys[i] % SM; if (!sset[k]) { sset[k] = 1; size3++; } }
        lsum = 0;
        for (int i = 0; i < N; i++) lsum += keys[i] % 1000;
        acc = (acc * 31 + size1) % Q;
        acc = (acc * 31 + found) % Q;
        acc = (acc * 31 + vsum) % Q;
        acc = (acc * 31 + mchk) % Q;
        acc = (acc * 31 + size2) % Q;
        acc = (acc * 31 + size3) % Q;
        acc = (acc * 31 + lsum) % Q;
    }
    printf("MB11 %lld %lld %lld %lld %lld %lld %lld %lld\n", acc, size1, found, vsum, mchk, size2, size3, lsum);
    free(keys); free(cnt); free(present); free(sset);
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : "all";
    #define RUN(tag, fn) if (!strcmp(only, "all") || !strcmp(only, tag)) { fn(); fflush(stdout); }
    RUN("mb01", mb01) RUN("mb02", mb02) RUN("mb03", mb03) RUN("mb04", mb04)
    RUN("mb05", mb05) RUN("mb06", mb06) RUN("mb07", mb07) RUN("mb08", mb08)
    RUN("mb09", mb09) RUN("mb10", mb10) RUN("mb11", mb11)
    return 0;
}
