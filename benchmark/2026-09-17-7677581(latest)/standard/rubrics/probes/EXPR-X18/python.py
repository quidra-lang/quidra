import os
open("x18.txt", "w").write("hello")
data = open("x18.txt").read()
print("X18", data, str(os.path.exists("x18.txt")).lower())
