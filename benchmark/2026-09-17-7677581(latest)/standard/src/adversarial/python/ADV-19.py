import ctypes


def f(n: ctypes.c_int64) -> ctypes.c_int64:
    return ctypes.c_int64(1 + f(ctypes.c_int64(n.value + 1)).value)


print("ADV-START", flush=True)
r = f(ctypes.c_int64(int(input())))
print("OBS=R:" + str(r.value), flush=True)
print("ADV-END", flush=True)
