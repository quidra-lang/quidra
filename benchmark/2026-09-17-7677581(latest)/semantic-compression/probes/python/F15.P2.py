# BEGIN PROBE F15.P2
def max_of[T](a: T, b: T) -> T:
    return a if a > b else b


m = max_of(3, 5)
# END PROBE F15.P2

print(m)
