import ctypes
print("ADV-START", flush=True)
a = ctypes.c_int64(7)
b = ctypes.c_int64(0)
q = ctypes.c_int64(a.value // b.value)
print("OBS=QUOT:" + str(q.value), flush=True)
print("ADV-END", flush=True)
