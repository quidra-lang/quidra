def probe() -> int:
    xs = [1, 2, 3]
    k = 0
    # BEGIN PROBE F05.P3
    def neg(a):
        return k - a

    ys = list(map(neg, xs))
    return ys[0]
    # END PROBE F05.P3


print(probe())
