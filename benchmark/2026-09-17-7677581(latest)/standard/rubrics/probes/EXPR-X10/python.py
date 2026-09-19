class V:
    def __init__(self, x, y): self.x, self.y = x, y
    def __add__(self, o): return V(self.x + o.x, self.y + o.y)
v = V(1, 2) + V(3, 4)
print("X10", v.x, v.y)
