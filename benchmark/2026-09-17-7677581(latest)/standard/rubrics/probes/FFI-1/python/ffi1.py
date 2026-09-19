import ctypes
from ctypes.util import find_library

libm = ctypes.CDLL(find_library("m"))
libm.cos.argtypes = [ctypes.c_double]
libm.cos.restype = ctypes.c_double
print("cos(1.0)=%.10f" % libm.cos(1.0))
