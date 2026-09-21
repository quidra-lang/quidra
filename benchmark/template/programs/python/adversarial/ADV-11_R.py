import ctypes
print("ADV-START", flush=True)
n = ctypes.c_int64(int(input()))
xs = [ctypes.c_int64(0)] * n.value
xs[0] = ctypes.c_int64(1)
first = xs[0]
print("OBS=ALLOC:" + str(len(xs)) + "|FIRST:" + str(first.value), flush=True)
print("ADV-END", flush=True)
