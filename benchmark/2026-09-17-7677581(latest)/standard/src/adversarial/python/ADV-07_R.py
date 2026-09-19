print("ADV-START", flush=True)
a = float(input())
b = float(input())
h = a / b
mean = (1.0 + h + 3.0) / 3.0
diff = h - h
print("OBS=MEAN:" + format(mean, ".6f") + "|DIFF:" + format(diff, ".6f"), flush=True)
print("ADV-END", flush=True)
