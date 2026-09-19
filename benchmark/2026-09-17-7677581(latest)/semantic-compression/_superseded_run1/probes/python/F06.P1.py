def main() -> int:
# BEGIN PROBE F06.P1
    def mid(xs: list[int]) -> int:
        return xs[1]

    xs = [1, 2, 3]
    y = mid(xs)
    return y
# END PROBE F06.P1


print(main())
