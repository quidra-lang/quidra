# CONC-1, Python. Frozen workload: sum of sin(i)*sin(i) for i in 0..N.
# N = 500000000, split into 4 fixed contiguous chunks. W workers take chunks
# c with c % W == w, so the per-chunk partial sums and the final combination
# order are IDENTICAL at W=1 and W=4 and the printed result is bit-identical.
# Worker mechanism: multiprocessing.Process (CPython standard library).
import math
import sys
from multiprocessing import Process, Pipe

N = 500000000
CHUNKS = 4
SPAN = N // CHUNKS


def chunk_sum(c):
    s = 0.0
    start = c * SPAN
    end = start + SPAN
    for i in range(start, end):
        x = math.sin(i)
        s += x * x
    return s


def worker(w, W, conn):
    out = {}
    for c in range(CHUNKS):
        if c % W == w:
            out[c] = chunk_sum(c)
    conn.send(out)
    conn.close()


def main():
    W = 1
    a = sys.argv[1:]
    for k in range(len(a)):
        if a[k] == "--workers":
            W = int(a[k + 1])
    partial = [0.0] * CHUNKS
    procs = []
    conns = []
    for w in range(W):
        pr, pw = Pipe(False)
        p = Process(target=worker, args=(w, W, pw))
        p.start()
        pw.close()
        procs.append(p)
        conns.append(pr)
    for pr in conns:
        for c, v in pr.recv().items():
            partial[c] = v
    for p in procs:
        p.join()
    total = 0.0
    for c in range(CHUNKS):
        total = total + partial[c]
    print("workers=%d result=%.10f" % (W, total))


if __name__ == "__main__":
    main()
