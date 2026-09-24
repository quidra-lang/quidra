import ctypes
print("ADV-START", flush=True)
x = 1e30
v = ctypes.c_int32(x)
print("OBS=V:" + str(v.value), flush=True)
print("ADV-END", flush=True)
