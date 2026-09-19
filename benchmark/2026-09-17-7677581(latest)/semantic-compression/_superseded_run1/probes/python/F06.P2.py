def main() -> int:
# BEGIN PROBE F06.P2
    def divmod2(a: int, b: int) -> tuple[int, int]:
        return a // b, a % b

    q, r = divmod2(7, 3)
    return q + r
# END PROBE F06.P2


print(main())
