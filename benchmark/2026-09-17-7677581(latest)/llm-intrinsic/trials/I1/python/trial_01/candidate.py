def collect(count: int) -> list[int]:
    out: list[int] = []
    state: int = 7
    for step in range(count):
        state = (state * 48271) % 2147483647
        out.append(state % 1000)
    return out


def bigger(left: int, right: int) -> int:
    if left > right:
        return left
    else:
        return right


def main() -> None:
    items: list[int] = collect(50)
    total: int = 0
    biggest: int = items[0]
    evens: int = 0
    for item in items:
        total = total + item
        biggest = bigger(biggest, item)
        if item % 2 == 0:
            evens = evens + 1
    pieces: list[str] = []
    for index in range(5):
        pieces.append(str(items[index]))
    label: str = "-".join(pieces)
    print("SUM " + str(total))
    print("MAX " + str(biggest))
    print("EVENS " + str(evens))
    print("JOINED " + label)


main()
