def fallback() -> int:
    # BEGIN PROBE F12.P1
    o: int | None = None
    n = 0 if o is None else o
    return n
    # END PROBE F12.P1


print(fallback())
