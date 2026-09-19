def set_second() -> int:
    xs = [10, 20, 30]
    # BEGIN PROBE F03.P2
    xs[1] = 42
    # END PROBE F03.P2
    return xs[1]


print(set_second())
