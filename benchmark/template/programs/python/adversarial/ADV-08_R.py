import ctypes
print("ADV-START", flush=True)
a = ctypes.c_int64(int(input()))
b = ctypes.c_int64(int(input()))
q = ctypes.c_int64(a.value // b.value)
print("OBS=QUOT:" + str(q.value), flush=True)
print("ADV-END", flush=True)
