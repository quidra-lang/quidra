# BEGIN PROBE F20.P1
import ctypes

libc = ctypes.CDLL(None)
libc.abs.argtypes = [ctypes.c_int]
libc.abs.restype = ctypes.c_int
magnitude = libc.abs(-3)
# END PROBE F20.P1


print(magnitude)
