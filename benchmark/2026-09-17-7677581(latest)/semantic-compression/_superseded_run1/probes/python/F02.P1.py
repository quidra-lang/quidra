def pick(cond: bool) -> int:
    # BEGIN PROBE F02.P1
    v: int
    if cond:
        v = 5
    else:
        v = 9
    return v
    # END PROBE F02.P1


print(pick(True))
