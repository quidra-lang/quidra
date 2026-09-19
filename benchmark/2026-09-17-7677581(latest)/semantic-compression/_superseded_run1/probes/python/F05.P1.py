def f(items: list[int]) -> None:
    items[0] = 99


def call_f() -> int:
    # BEGIN PROBE F05.P1
    x = [1, 2, 3]
    f(x)
    return x[0]
    # END PROBE F05.P1


print(call_f())
