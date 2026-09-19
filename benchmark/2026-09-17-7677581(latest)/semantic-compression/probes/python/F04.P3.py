class Box:
    def __init__(self, id: int) -> None:
        self.id = id


def probe() -> bool:
    # BEGIN PROBE F04.P3
    first = Box(5)
    second = [first][0]
    same = first is second
    return same
    # END PROBE F04.P3


print(probe())
