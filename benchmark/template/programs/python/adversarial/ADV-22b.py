import ctypes
print("ADV-START", flush=True)
v: ctypes.c_int64 = ctypes.c_int@#$64(1)
print("OBS=V:" + str(v.value), flush=True)
print("ADV-END", flush=True)
