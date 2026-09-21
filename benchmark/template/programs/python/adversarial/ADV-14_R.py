import ctypes


def scale(n: ctypes.c_int64) -> ctypes.c_int64:
    return n * 3


print("ADV-START", flush=True)
t = input()
r = scale(t)
print("OBS=R:" + str(r), flush=True)
print("ADV-END", flush=True)
