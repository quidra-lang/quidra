import ctypes


def pick(b: bool) -> ctypes.c_int64:
    if b:
        return 1


print("ADV-START", flush=True)
b = input() == "1"
r = pick(b) * 2
print("OBS=R:" + str(r), flush=True)
print("ADV-END", flush=True)
