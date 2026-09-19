def main() -> str:
    xs = [3, 1, 2]
    a = 4
    b = 9
# BEGIN PROBE F09.P2
    xs.sort()
    lt = a < b
# END PROBE F09.P2
    return f"{xs} {lt}"


print(main())
