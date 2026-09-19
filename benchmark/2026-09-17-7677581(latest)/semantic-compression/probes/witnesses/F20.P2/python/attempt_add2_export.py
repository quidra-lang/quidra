# Attempt: publish a C-ABI function named exactly `add2` from Python using only
# the standard library's built-in FFI mechanism (ctypes).
import ctypes

PROTO = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_int)


def add2(a, b):
    return a + b


cb = PROTO(add2)
print("callable through a C function pointer:", cb(2, 3))
print("address:", hex(ctypes.cast(cb, ctypes.c_void_p).value))
print("any exported symbol named add2:", hasattr(ctypes.CDLL(None), "add2"))
