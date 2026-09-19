import ctypes
print("ADV-START", flush=True)
s = ctypes.c_int32(-1)
u = ctypes.c_uint32(1)
r = s.value < u.value
print("OBS=CMP:" + str(r).lower(), flush=True)
print("ADV-END", flush=True)
