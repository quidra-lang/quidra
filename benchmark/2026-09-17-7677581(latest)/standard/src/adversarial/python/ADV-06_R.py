print("ADV-START", flush=True)
a = float(input())
b = float(input())
m = a / b
xs = [3.0, m, 1.0]
best = xs[0]
for x in xs[1:]:
    if x > best:
        best = x
print("OBS=MAX:" + format(best, ".6f") + "|SELFEQ:" + str(m == m).lower(), flush=True)
print("ADV-END", flush=True)
