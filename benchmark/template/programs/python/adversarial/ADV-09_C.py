import ctypes
print("ADV-START", flush=True)
xs = [ctypes.c_int64(10), ctypes.c_int64(20), ctypes.c_int64(30), ctypes.c_int64(40), ctypes.c_int64(50)]
i = -1
e = xs[i]
print("OBS=ELEM:" + str(e.value), flush=True)
print("ADV-END", flush=True)
