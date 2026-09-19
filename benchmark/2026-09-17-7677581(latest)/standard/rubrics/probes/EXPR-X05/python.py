def make_adder(n):
    def add(x):
        return x + n
    return add
f = make_adder(10)
print("X05", f(5))
