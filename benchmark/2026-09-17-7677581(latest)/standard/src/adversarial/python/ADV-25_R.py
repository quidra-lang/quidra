import ctypes

print("ADV-START", flush=True)
t = input()
n = ctypes.c_int64(int(t))
r = ctypes.c_int64(n.value * 2)
print("OBS=R:" + str(r.value), flush=True)
print("ADV-END", flush=True)
