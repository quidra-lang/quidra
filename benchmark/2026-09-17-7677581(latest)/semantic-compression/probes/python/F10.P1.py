from ctypes import c_int32


def narrow(big):
    # BEGIN PROBE F10.P1
    small = c_int32(big)
    return small
    # END PROBE F10.P1


print(narrow(2147483648).value)
