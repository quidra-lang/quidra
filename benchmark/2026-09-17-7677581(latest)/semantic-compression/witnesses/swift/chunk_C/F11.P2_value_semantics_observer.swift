var xs = [10, 20, 30, 40, 50]
let part = xs[1..<4]
xs[1] = 99
print(part.first!, xs[1])
