import ctypes


def main():
    print("ADV-START", flush=True)
    X: ctypes.c_int64 = ctypes.c_int64(10)
    X = ctypes.c_int64(20)
    print("OBS=V:" + str(X.value), flush=True)
    print("ADV-END", flush=True)


main()
