import ctypes


def read_uninitialized():
    v: ctypes.c_int64
    print("OBS=VAL:" + str(v.value), flush=True)


print("ADV-START", flush=True)
read_uninitialized()
print("ADV-END", flush=True)
