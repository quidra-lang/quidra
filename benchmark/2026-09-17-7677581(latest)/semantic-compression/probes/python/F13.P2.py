def handled(s: str) -> int:
    # BEGIN PROBE F13.P2
    try:
        n = int(s)
    except ValueError:
        n = 0
    return n
    # END PROBE F13.P2


print(handled("21"))
