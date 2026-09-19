import ctypes
import typing


class A:
    def __init__(self, v):
        self.v = v


class B:
    def __init__(self, v):
        self.v = v


print("ADV-START", flush=True)
a = A(ctypes.c_int64(42))
o: object = a
b = typing.cast(B, o)
print("OBS=V:" + str(b.v.value), flush=True)
print("ADV-END", flush=True)
