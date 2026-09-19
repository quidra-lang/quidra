func makeAdder(_ n: Int) -> (Int) -> Int { { x in x + n } }
print("X05", makeAdder(10)(5))
