var buf = [0, 0, 0, 0]
buf.withUnsafeMutableBufferPointer { b in
  let view = UnsafeMutableBufferPointer(rebasing: b[1..<3])   // non-copying view
  view[0] = 99; view[1] = 99
}
print("X23", buf[1], buf[2])
