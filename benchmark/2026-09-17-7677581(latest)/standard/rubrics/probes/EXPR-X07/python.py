class Upto:
    def __init__(self, n): self.n = n
    def __iter__(self):
        i = 0
        while i < self.n:
            yield i
            i += 1
vals = []
for v in Upto(3):
    vals.append(str(v))
print("X07", " ".join(vals))
