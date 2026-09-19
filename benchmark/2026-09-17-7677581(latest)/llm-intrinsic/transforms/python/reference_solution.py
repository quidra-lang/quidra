def generate(count: int) -> list[int]:
    terms: list[int] = []
    state: int = 7
    for step in range(count):
        state = (state * 48271) % 2147483647
        terms.append(state % 1000)
    return terms


def main() -> None:
    terms: list[int] = generate(50)
    total: int = 0
    largest: int = terms[0]
    evens: int = 0
    for term in terms:
        total = total + term
        if term > largest:
            largest = term
        if term % 2 == 0:
            evens = evens + 1
    pieces: list[str] = []
    for index in range(5):
        pieces.append(str(terms[index]))
    joined: str = "-".join(pieces)
    print("SUM " + str(total))
    print("MAX " + str(largest))
    print("EVENS " + str(evens))
    print("JOINED " + joined)


main()
