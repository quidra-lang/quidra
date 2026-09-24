import ctypes
print("ADV-START", flush=True)
x = float(input())
v = ctypes.c_int32(x)
print("OBS=V:" + str(v.value), flush=True)
print("ADV-END", flush=True)
