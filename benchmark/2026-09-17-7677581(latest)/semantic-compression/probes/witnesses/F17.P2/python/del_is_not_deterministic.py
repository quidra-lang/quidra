# Witness for the P-a substitution at F17.P2: Python's own end-of-lifetime hook
# (object.__del__) is NOT a deterministic release point, so the probe's named
# substitution (an explicit dispose invoked by a scoped construct) is used instead.
released = 0


class Handle:
    def __init__(self, id):
        self.id = id

    def __del__(self):
        global released
        released += 1


def block():
    h = Handle(1)
    h.self = h          # a reference cycle: reachable only by the cycle collector


block()
print("released immediately after the block ended:", released)
import gc
gc.collect()
print("released after an explicit gc.collect():", released)
