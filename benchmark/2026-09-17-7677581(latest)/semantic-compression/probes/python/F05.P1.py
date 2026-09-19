def f(v: list[int]) -> int:
    return len(v)


def probe() -> int:
    # BEGIN PROBE F05.P1
    x = [1, 2, 3]
    f(x)
    return x[0]
    # END PROBE F05.P1


print(probe())
