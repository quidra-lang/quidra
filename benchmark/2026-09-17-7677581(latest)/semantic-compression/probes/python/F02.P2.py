def probe() -> int:
    # BEGIN PROBE F02.P2
    buf = bytearray(16)
    buf[0] = 1
    return buf[0]
    # END PROBE F02.P2


print(probe())
