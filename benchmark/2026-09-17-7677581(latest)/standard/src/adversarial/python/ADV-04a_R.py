import ctypes
print("ADV-START", flush=True)
a = ctypes.c_uint32(int(input()))
b = ctypes.c_uint32(int(input()))
r = ctypes.c_uint32(a.value - b.value)
print("OBS=SUB:" + str(r.value), flush=True)
print("ADV-END", flush=True)
