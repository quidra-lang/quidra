def collect(count: int) -> list[int]:
    out: list[int] = []
    value: int = 3
    for step in range(count):
        value = (value * 11) % 97
        out.append(value)
    return out


def bigger(left: int, right: int) -> int:
    if left > right:
        return left
    else:
        return right


def main() -> None:
    items: list[int] = collect(6)
    total: int = 0
    biggest: int = items[0]
    odds: int = 0
    for item in items:
        total = total + item
        biggest = bigger(biggest, item)
        if item % 2 == 1:
            odds = odds + 1
    pieces: list[str] = []
    for index in range(3):
        pieces.append(str(items[index]))
    label: str = ":".join(pieces)
    print("TOTAL " + str(total))
    print("BIGGEST " + str(biggest))
    print("ODDS " + str(odds))
    print("LABEL " + label)


main()
