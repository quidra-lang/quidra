def caller(a, b):
    # BEGIN PROBE F08.P2
    q = 0 if b == 0 else a // b
    return q
    # END PROBE F08.P2


print(caller(-7, 2), caller(7, 0))
