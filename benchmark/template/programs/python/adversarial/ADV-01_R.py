import ctypes
print("ADV-START", flush=True)
src = ctypes.c_int64(int(input()))
dst = ctypes.c_int32(src.value)
print("OBS=V:" + str(dst.value), flush=True)
print("ADV-END", flush=True)
