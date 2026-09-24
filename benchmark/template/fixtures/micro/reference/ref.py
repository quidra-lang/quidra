#!/usr/bin/env python3
# Independent Python implementation of the 11 frozen micro workloads,
# written from the frozen prose spec, used to cross-validate the C reference.
import math, os, sys

P = 2147483647
A_MUL = 48271
MAXEXACT = 2**53
_bound = 0


def bound(v):
    global _bound
    if v > _bound:
        _bound = v
    return v


class R:
    def __init__(self, seed):
        self.s = seed

    def i(self):
        bound(A_MUL * self.s)
        self.s = (A_MUL * self.s) % P
        return self.s

    def u(self):
        return self.i() / 2147483647.0


def fmt(x):
    return "%.16e" % x


def mb01():
    sys.setrecursionlimit(100000)
    def fib(n):
        return n if n < 2 else fib(n - 1) + fib(n - 2)
    t = 0
    for n in range(30, 38):
        t += fib(n)
    print("MB01 %d" % t, flush=True)


def mb02():
    M, N = 1000003, 20000
    total = 0
    for k in range(1, N + 1):
        f = 1
        for j in range(2, k + 1):
            bound(f * j)
            f = (f * j) % M
        total = (total + f) % M
    print("MB02 %d" % total, flush=True)


def mb03():
    N = 120000000
    x = 20263917
    sa = sx = sd = 0
    sm = 1
    for _ in range(N):
        x = (48271 * x) % 2147483647
        sa = (sa + x) % 2147483647
        sx = sx ^ x
        sm = (sm * 33 + (x % 97)) % 1000003
        sd = sd + (x // 1000)
    bound(sd)
    print("MB03 %d %d %d %d" % (sa, sx, sm, sd), flush=True)


def mb04():
    M, RR = 4000, 75000
    g = R(20264917)
    A = [0.5 + g.u() for _ in range(M)]
    B = [0.5 + g.u() for _ in range(M)]
    s1 = s2 = s3 = s4 = 0.0
    for r in range(RR):
        A[r % M] = A[r % M] + 1.0e-9
        for i in range(M):
            a = A[i]; b = B[i]
            s1 = s1 + a * b
            s2 = s2 + a / (b + 2.0)
            s3 = s3 + math.sqrt(a * a + b * b)
            s4 = s4 + (a - b) * (a - b)
    print("MB04 s1=%s s2=%s s3=%s s4=%s" % (fmt(s1), fmt(s2), fmt(s3), fmt(s4)), flush=True)


def mb05():
    N, RR = 2000000, 400
    g = R(20265917)
    X = [0.5 + g.u() for _ in range(N)]
    Y = [0.5 + g.u() for _ in range(N)]
    total = 0.0
    for r in range(RR):
        X[r] = X[r] + 1.0e-9
        d = 0.0
        for i in range(N):
            d = d + X[i] * Y[i]
        total = total + d
    print("MB05 total=%s" % fmt(total), flush=True)


def mb06():
    n, RR = 512, 3
    g = R(20266917)
    A = [g.u() for _ in range(n * n)]
    B = [g.u() for _ in range(n * n)]
    C = [0.0] * (n * n)
    for r in range(RR):
        A[r] = A[r] + 1.0e-9
        for i in range(n):
            for j in range(n):
                s = 0.0
                for k in range(n):
                    s = s + A[i * n + k] * B[k * n + j]
                C[i * n + j] = s
    tot = 0.0
    for v in C:
        tot = tot + v
    print("MB06 sumC=%s c_first=%s c_last=%s" % (fmt(tot), fmt(C[0]), fmt(C[n * n - 1])), flush=True)


def msort(a, buf, n):
    src, dst = a, buf
    width = 1
    while width < n:
        lo = 0
        while lo < n:
            mid = min(lo + width, n)
            hi = min(lo + 2 * width, n)
            i, j, k = lo, mid, lo
            while i < mid and j < hi:
                if src[i] <= src[j]:
                    dst[k] = src[i]; i += 1
                else:
                    dst[k] = src[j]; j += 1
                k += 1
            while i < mid:
                dst[k] = src[i]; i += 1; k += 1
            while j < hi:
                dst[k] = src[j]; j += 1; k += 1
            lo += 2 * width
        src, dst = dst, src
        width *= 2
    if src is not a:
        for i in range(n):
            a[i] = src[i]


def mb07():
    N, RR = 2000000, 4
    g = R(20267917)
    src = [g.i() for _ in range(N)]
    buf = [0] * N
    total = 0; ssum = 0; inv = 0
    for r in range(RR):
        src[r] = src[r] + 1
        a = list(src)
        msort(a, buf, N)
        chk = 0
        for i in range(N):
            bound(chk * 31)
            chk = (chk * 31 + (a[i] % 1000003)) % 1000003
        total = (total * 7 + chk) % 1000003
        ssum = 0
        for v in a:
            ssum += v
        bound(ssum)
        for i in range(1, N):
            if a[i - 1] > a[i]:
                inv += 1
    print("MB07 %d %d %d" % (total, ssum, inv), flush=True)


def rhash(s):
    h = 0
    for c in s:
        bound(h * 131 + c)
        h = (h * 131 + c) % 1000000007
    return h


def mb08():
    NW, RR = 200000, 20
    g = R(20268917)
    parts = []
    for w in range(NW):
        L = 4 + (g.i() % 13)
        parts.append(bytes(97 + (g.i() % 26) for _ in range(L)))
    text = bytearray(b" ".join(parts))
    ln = len(text)
    Q = 1000000007
    acc = 0; cnt_ab = 0; cnt_w = 0
    for r in range(RR):
        p = 7 * r + 11
        if text[p] == 32:
            text[p] = ord('x')
        else:
            text[p] = 97 + ((text[p] - 97 + 1) % 26)
        h1 = rhash(text)
        U = bytearray(ln)
        for i in range(ln):
            c = text[i]
            U[i] = c - 32 if 97 <= c <= 122 else c
        h2 = rhash(U)
        Rv = bytearray(ln)
        for i in range(ln):
            Rv[i] = text[ln - 1 - i]
        h3 = rhash(Rv)
        cnt_ab = 0
        for i in range(ln - 1):
            if text[i] == 97 and text[i + 1] == 98:
                cnt_ab += 1
        cnt_w = 1
        for i in range(ln):
            if text[i] == 32:
                cnt_w += 1
        for v in (h1, h2, h3, cnt_ab, cnt_w):
            bound(acc * 31 + v)
            acc = (acc * 31 + v) % Q
    print("MB08 %d %d %d %d" % (acc, ln, cnt_ab, cnt_w), flush=True)


def mb09():
    N, RR = 2000000, 30
    g = R(20269917)
    X = [g.u() * 100.0 for _ in range(N)]
    Y = [g.u() * 100.0 for _ in range(N)]
    for r in range(RR):
        X[r] = X[r] + 1.0e-9
        s = 0.0; mn = X[0]; mx = X[0]
        for v in X:
            s = s + v
            if v < mn: mn = v
            if v > mx: mx = v
        mean = s / N
        sq = 0.0; ad = 0.0
        for v in X:
            d = v - mean
            sq = sq + d * d
            ad = ad + (-d if d < 0 else d)
        var = sq / N; sd = math.sqrt(var); mad = ad / N
        sy = 0.0
        for v in Y:
            sy = sy + v
        meany = sy / N
        sxy = 0.0; sxx = 0.0; syy = 0.0
        for i in range(N):
            dx = X[i] - mean; dy = Y[i] - meany
            sxy = sxy + dx * dy; sxx = sxx + dx * dx; syy = syy + dy * dy
        pear = sxy / math.sqrt(sxx * syy)
        hist = [0] * 64
        for v in X:
            b = int(math.floor(v * 0.64))
            if b < 0: b = 0
            if b > 63: b = 63
            hist[b] += 1
        hchk = 0
        for b in range(64):
            hchk += (b + 1) * hist[b]
    print("MB09 mean=%s var=%s sd=%s min=%s max=%s mad=%s pearson=%s hist_chk=%d"
          % (fmt(mean), fmt(var), fmt(sd), fmt(mn), fmt(mx), fmt(mad), fmt(pear), hchk), flush=True)


def mb10():
    N, RR = 1000000, 3
    g = R(20270917)
    sum_v = 0; chk = 0; nbytes = 0; lines = 0
    for r in range(RR):
        name = "mb10_round_%d.txt" % r
        with open(name, "w", encoding="ascii") as f:
            for i in range(N):
                v = g.i()
                line = "%d %d\n" % (i, v)
                f.write(line)
                nbytes += len(line)
        with open(name, "r", encoding="ascii") as f:
            idx = 0
            for line in f:
                a, b = line.split(" ")
                ai = int(a); v = int(b)
                assert ai == idx
                idx += 1; lines += 1
                sum_v = (sum_v + v) % 1000000007
                chk = (chk * 31 + (v % 1000003)) % 1000003
    print("MB10 %d %d %d %d" % (sum_v, chk, nbytes, lines), flush=True)


def mb11():
    N, RR = 1000000, 3
    KM, SM, Q = 500009, 100003, 1000000007
    g = R(20271917)
    keys = [g.i() for _ in range(N)]
    acc = 0
    for r in range(RR):
        keys[r] = keys[r] + 1000000
        m = {}
        for i in range(N):
            k = keys[i] % KM
            m[k] = m.get(k, 0) + 1
        size1 = len(m)
        found = 0; vsum = 0
        for i in range(N):
            k = (keys[i] + 7) % KM
            if k in m:
                found += 1
                vsum += m[k]
        mchk = 0
        for k, v in m.items():
            bound((k % 1000003) * v)
            mchk = (mchk + (k % 1000003) * v) % 1000003
        for i in range(0, N, 2):
            k = keys[i] % KM
            if k in m:
                del m[k]
        size2 = len(m)
        st = set()
        for i in range(N):
            st.add(keys[i] % SM)
        size3 = len(st)
        lst = []
        for i in range(N):
            lst.append(keys[i] % 1000)
        lsum = 0
        for v in lst:
            lsum += v
        for v in (size1, found, vsum, mchk, size2, size3, lsum):
            bound(acc * 31 + v)
            acc = (acc * 31 + v) % Q
    print("MB11 %d %d %d %d %d %d %d %d" % (acc, size1, found, vsum, mchk, size2, size3, lsum), flush=True)


ALL = [mb01, mb02, mb03, mb04, mb05, mb06, mb07, mb08, mb09, mb10, mb11]
if __name__ == "__main__":
    want = sys.argv[1] if len(sys.argv) > 1 else "all"
    import time
    for f in ALL:
        if want == "all" or want == f.__name__:
            t0 = time.perf_counter()
            f()
            print("    # %s python %.2fs" % (f.__name__, time.perf_counter() - t0), flush=True)
    print("# max integer magnitude seen = %d (2^53 = %d) ok=%s" % (_bound, MAXEXACT, _bound < MAXEXACT), flush=True)
