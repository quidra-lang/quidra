# CONC-2: two concurrent workers increment a shared counter with NO synchronization.
import threading

ITERS = 200000
counter = 0


def bump():
    global counter
    for _ in range(ITERS):
        counter = counter + 1


t1 = threading.Thread(target=bump)
t2 = threading.Thread(target=bump)
t1.start(); t2.start(); t1.join(); t2.join()
print("counter=%d expected=%d" % (counter, 2 * ITERS))
