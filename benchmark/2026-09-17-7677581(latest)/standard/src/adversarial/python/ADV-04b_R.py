import ctypes
print("ADV-START", flush=True)
s = ctypes.c_int32(int(input()))
u = ctypes.c_uint32(int(input()))
r = s.value < u.value
print("OBS=CMP:" + str(r).lower(), flush=True)
print("ADV-END", flush=True)
