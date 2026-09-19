import ctypes
print("ADV-START", flush=True)
a = ctypes.c_uint32(0)
b = ctypes.c_uint32(1)
r = ctypes.c_uint32(a.value - b.value)
print("OBS=SUB:" + str(r.value), flush=True)
print("ADV-END", flush=True)
