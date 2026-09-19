# BEGIN PROBE F15.P1
def head[T](values: list[T]) -> T:
    return values[0]


a = head([4, 5, 6])
b = head(["p", "q"])
result = str(a) + b
# END PROBE F15.P1

print(result)
