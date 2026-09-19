def transform() -> int:
    o: int | None = None
    # BEGIN PROBE F12.P2
    def h(v):
        return v + 1

    p: int | None = None if o is None else h(o)
    return 0 if p is None else p
    # END PROBE F12.P2


print(transform())
