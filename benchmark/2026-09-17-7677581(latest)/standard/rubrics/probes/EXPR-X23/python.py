buf = memoryview(bytearray(b"\x00\x00\x00\x00"))
view = buf[1:3]
view[0] = 99
view[1] = 99
print("X23", buf[1], buf[2])
