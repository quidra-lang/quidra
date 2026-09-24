import ctypes

print("ADV-START", flush=True)
xs = [ctypes.c_int64(int(input())) for _ in range(7)]
target = ctypes.c_int64(int(input()))
lo = 0
hi = 6
result = -1
while lo <= hi:
    mid = (lo + hi) // 2
    if xs[mid].value == target.value:
        result = mid
        break
    if xs[mid].value < target.value:
        lo = mid + 1
    else:
        hi = mid - 1
print("OBS=IDX:" + str(result), flush=True)
print("ADV-END", flush=True)
